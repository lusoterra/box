# box (Booru eXplorer)

Booru client for local archives and online boorus, as a native C++/Qt6 app.
Point it at a folder of downloaded posts, browse Danbooru/Gelbooru and more
online, download in full quality, and keep favourites — no browser needed.

Local archives are treated read-only: browsing never modifies your library.

## Supported boorus

<details>
<summary><b>Engines and example instances</b> (click to expand)</summary>

| Engine | Example instances | Notes |
| --- | --- | --- |
| Danbooru | danbooru.donmai.us | API keys supported |
| Gelbooru | Safebooru (ships as the default), Gelbooru, Rule34, AIBoru | Rule34 needs API credentials |
| Moebooru | Konachan, Yandere | |
| Szurubooru | any Szurubooru instance | |
| Shimmie | any Shimmie instance | |
| HyBooru | any HyBooru instance | |
| Nuuru | soybooru.com | proof-of-work clearance solves itself in-app |
| HiddenBooru | Tor onion service (+ slow I2P mirror) | needs a Tor proxy (`socks5h://127.0.0.1:9050`) |

Any instance of a supported engine can be added in Preferences → Booru
manager (base URL + engine type); only Safebooru ships preconfigured so
fresh installs start SFW-only.
</details>

## Requirements

- C++17 compiler (GCC, Clang, MSVC)
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


## Local archives

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
