# ---------------------------------------------------------------------------
# One place to build and test everything locally.
#
#   make              build the 3DS programs (needs devkitPro with 3ds-dev):
#                     hello3d.3dsx and platinum-smoke.3dsx
#   make game         the game for the 3DS: 3ds_port/game/build/3ds/pokeplatinum.3dsx
#                     (runs with your own data pack: 3ds_port/game/README.md)
#   make game-cia ROM=platinum.nds
#                     the same, installable on the HOME Menu: .../pokeplatinum.cia
#                     (its icon from your ROM; builds makerom and bannertool once)
#   make test         build for the PC and run every test suite
#   make link-check   link the whole game for the 3DS and report what's missing
#   make clean
#
# Run `python tools/bootstrap.py` once first (it fetches the decompilation
# and the SDKs). Builds that contain game code (platinum-smoke.3dsx, and the
# game later) are for your own use: CI builds them to check they work but
# never publishes them, and they must never be committed or shared
# (CONTRIBUTING.md).
# ---------------------------------------------------------------------------

PORT := 3ds_port

.PHONY: all 3ds hello3d smoke game game-cia test link-check clean check-bootstrap

all: 3ds

3ds: hello3d smoke
	@echo
	@echo "Built:"
	@echo "  $(PORT)/hello3d/hello3d.3dsx"
	@echo "  $(PORT)/platinum_smoke/platinum-smoke.3dsx   (contains game code: don't share it)"
	@echo "Open them in Azahar, or send one to a 3DS with: 3dslink <file>.3dsx"

check-bootstrap:
	@test -f build/upstream/include/pch/global_pch.h || \
	    { echo "Run first: python tools/bootstrap.py"; exit 1; }

hello3d:
	@$(MAKE) --no-print-directory -C $(PORT)/hello3d

# The smoke app's Makefile builds libnitro, libnitrosystem and the game
# library for the 3DS first.
smoke: check-bootstrap
	@$(MAKE) --no-print-directory -C $(PORT)/platinum_smoke

game: check-bootstrap
	@$(MAKE) --no-print-directory -C $(PORT)/game TARGET=3ds program
	@echo "Built $(PORT)/game/build/3ds/pokeplatinum.3dsx (contains game code: don't share it)"
	@echo "It needs platinum3ds.pak and manifest.json in sdmc:/3ds/pokeplatinum/ (see $(PORT)/game/README.md)"

# The HOME Menu version. Its art (from your ROM's icon) and the tools go in
# build/: never committed.
CIA_TOOLS := $(CURDIR)/build/cia-tools
HOME_ART := $(CURDIR)/build/home-menu-art

game-cia: check-bootstrap
	@test -n "$(ROM)" || { echo "Usage: make game-cia ROM=path/to/your/platinum.nds"; exit 1; }
	@bash tools/build_cia_tools.sh $(CIA_TOOLS)
	@python3 tools/home_menu_art.py "$(ROM)" $(HOME_ART)
	@$(MAKE) --no-print-directory -C $(PORT)/game TARGET=3ds HOME_ART=$(HOME_ART) CIA_TOOLS=$(CIA_TOOLS) program cia
	@echo "Built $(PORT)/game/build/3ds/pokeplatinum.cia and .3dsx (contain game code: don't share them)"

test: check-bootstrap
	@$(MAKE) --no-print-directory -C $(PORT)/nitro
	@$(MAKE) --no-print-directory -C $(PORT)/nitrosystem
	@$(MAKE) --no-print-directory -C $(PORT)/pak
	@$(MAKE) --no-print-directory -C $(PORT)/game TARGET=host test program
	@$(PORT)/game/build/host/pokeplatinum-pc --check-overlays 2> build/overlay-check.log || \
	    { cat build/overlay-check.log; exit 1; }
	@tail -1 build/overlay-check.log
	python3 tools/boot_smoke.py $(PORT)/game/build/host/pokeplatinum-pc
	python3 -m unittest discover -s tools -p 'test_*.py'
	python3 -m unittest discover -s builder -p 'test_*.py'
	python3 tools/check_ci_artifacts.py

link-check: check-bootstrap
	@$(MAKE) --no-print-directory -C $(PORT)/game TARGET=3ds link-check

clean:
	@for d in hello3d nitro nitrosystem pak game platinum_smoke; do \
	    $(MAKE) --no-print-directory -C $(PORT)/$$d clean; done
