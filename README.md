# Box (BOoru eXplorer)

> Special thanks to the Belgian™ Government® for making me double
> down on everything that I believe in. Your pure incompetence is laughable,
> sad, and honestly, kind of pathetic.

Box (**Bo**oru e**x**plorer) is a Booru client for online boorus and local
*gallery-dl* and similar archives, as a native C++/Qt6 app. Point it at a folder
of downloaded posts, browse Danbooru/Gelbooru and more online, download in full
quality, and keep favourites.

![env](environment.jxl)

## Supported boorus

<details>
<summary><b>Engines and example instances</b> (click to expand)</summary>

| Engine | Example instances | Notes |
| --- | --- | --- |
| Danbooru | danbooru.donmai.us, safebooru.donmai.us (ships as the default) | API keys supported |
| Gelbooru | Gelbooru, Rule34, AIBoru, safebooru.org (add manually) | Rule34 needs API credentials |
| Moebooru | Konachan, Yandere | |
| Szurubooru | any Szurubooru instance | |
| Shimmie | any Shimmie instance | |
| HyBooru | any HyBooru instance | |
| Nuuru | soybooru.com | proof-of-work clearance solves itself in-app |
| HiddenBooru | Tor onion service (+ slow I2P mirror) | **Paused** - support is frozen and no longer offered in the UI; code/docs stay for existing entries. Tor proxy (`socks5h://127.0.0.1:9050`) or i2pd (`socks5h://127.0.0.1:4447`). |

Any instance of a supported engine can be added in Preferences → Booru
manager (base URL + engine type); only Safebooru ships preconfigured so
fresh installs start SFW-only. New booru entries default to the
`furry:g` default tag, which is expanded to `furry rating:general` on
the wire: furry posts stay allowed, but only general-rated ones by
default. Existing entries keep their own default tags.
</details>

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
make test   # or: ctest --test-dir build --output-on-failure
```

Debug builds: `-DCMAKE_BUILD_TYPE=Debug`. `make install` / `make uninstall` /
`make clean` wrap the obvious CMake steps.

For a local system install that always replaces older package/manual
copies (and removes a stale `/usr/local/bin/box` that would shadow
`/usr/bin/box`), use the incremental helper:

```sh
./scripts/install-latest.sh          # /usr
BOX_PREFIX=~/.local ./scripts/install-latest.sh
```

Pacman packaging lives in the separate `box-git` repo (PKGBUILD builds
from `git.lusosterone.net/lev/box`).

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

## Themes

Bundled themes live in `themes/*.ini` and are embedded into the binary
(reconfigure after adding one). You can also drop a file into
`~/.config/box/themes/` to add or override a theme without rebuilding.
Each file is a tiny `[theme]` INI with `name`, `order`, `dark`, and hex
colours (`window`, `base`, `alternate`, `button`, `text`, `dim`,
`disabled`, `border`, `accent`); see `themes/nord.ini` for the shape.

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
