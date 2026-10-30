# box packaging

`PKGBUILD` is a real `box-git` package (VCS source):
`git+https://git.lusosterone.net/lev/box.git`.

```sh
cd packaging
makepkg -sf
sudo pacman -U box-git-*.pkg.tar.zst
```

The package conflicts with/provides `box`, so it replaces an older
non-git install. `pkgver()` derives the version from the latest tag or
commit.

For local development, do **not** rebuild the package from scratch:
`scripts/install-latest.sh` (in the source repo) does an incremental
build with prefix `/usr`, installs over the package files, and removes
stale `/usr/local/bin/box` or `~/.local/bin/box` copies so the GUI
always starts the newest binary.
