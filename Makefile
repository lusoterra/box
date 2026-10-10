# Convenience wrapper around the CMake build.
#   make [build]        configure + compile (default)
#   make run [ARGS=..]  build and run from the build tree
#   make install        install to $(PREFIX) (DESTDIR supported)
#   make uninstall      remove installed files using the install manifest
#   make clean          drop the build tree
#
# Examples:
#   make
#   sudo make install
#   make install PREFIX=~/.local
#   make install DESTDIR=/tmp/pkg PREFIX=/usr
#   make run ARGS=~/pics/dump

PREFIX      ?= /usr/local
BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build configure run install uninstall clean

all: build

configure:
	cmake -B $(BUILD_DIR) -S . \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

run: build
	./$(BUILD_DIR)/booru-explorer $(ARGS)

install: build
	cmake --install $(BUILD_DIR)
	-update-desktop-database $(DESTDIR)$(PREFIX)/share/applications 2>/dev/null; true
	-gtk-update-icon-cache -q $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null; true
	@echo "installed under $(DESTDIR)$(PREFIX)"

uninstall:
	@if [ ! -f $(BUILD_DIR)/install_manifest.txt ]; then \
		echo "no install manifest in $(BUILD_DIR) (build first)"; exit 1; fi
	xargs -d '\n' -a $(BUILD_DIR)/install_manifest.txt rm -f
	@echo "uninstalled (icon/desktop caches may need a refresh)"

clean:
	rm -rf $(BUILD_DIR)
