# KenshiMP build / deploy / publish
#
# Requires: bash, make, VS2022 + v100 toolset (for core DLL), .NET 9 SDK.
# Paths are configured for a standard Steam install; override with:
#   make deploy KENSHI_MOD_DIR="/path/to/Kenshi/mods/KenshiMP"

# ---------------------------------------------------------------------------
# Shell — force GNU Make to use Git Bash on Windows.
#
# When invoked from PowerShell or cmd.exe, sh.exe is usually not on PATH,
# which makes Make blow up with:
#   process_begin: CreateProcess(NULL, sh.exe -c "...") failed.
#   make (e=2): The system cannot find the file specified.
#
# We locate Git for Windows via 8.3 short paths (no spaces -> no quoting
# nightmare with SHELL) and prepend its bin directory to PATH so any
# sub-process that itself shells out can find sh.exe too.
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
_BASH_CANDIDATES := \
    C:/PROGRA~1/Git/bin/bash.exe \
    C:/PROGRA~2/Git/bin/bash.exe
_FOUND_BASH := $(firstword $(wildcard $(_BASH_CANDIDATES)))
ifeq ($(_FOUND_BASH),)
$(error Could not find Git Bash. Install Git for Windows or run 'make' from a Git Bash shell.)
endif
SHELL := $(_FOUND_BASH)
.SHELLFLAGS := -c
export PATH := $(dir $(_FOUND_BASH)):$(PATH)
endif

# ---------------------------------------------------------------------------
# Config (override on the command line if your paths differ)
# ---------------------------------------------------------------------------
ROOT            := $(CURDIR)
VERSION         := 0.1.0

BUILD_CORE      := $(ROOT)/build
BUILD_SERVER    := $(ROOT)/build_server
GUI_DIR         := $(ROOT)/server/gui
DIST_DIR        := $(ROOT)/dist

ENET_DIR        ?= $(ROOT)/deps/enet2
KENSHILIB_DIR   ?= $(ROOT)/deps/KenshiLib
BOOST_ROOT      ?= $(ROOT)/deps/KenshiLib_Examples_deps/boost_1_60_0

KENSHI_MOD_DIR  ?= /c/Program Files (x86)/Steam/steamapps/common/Kenshi/mods/KenshiMP

CMAKE           ?= /c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe
DOTNET          ?= dotnet

# ---------------------------------------------------------------------------
# Human-readable banner
# ---------------------------------------------------------------------------
define BANNER
	@echo ""
	@echo "================================================================"
	@echo "  $(1)"
	@echo "================================================================"
endef

.PHONY: help all core server-core server-gui build deploy publish run-server clean distclean

help:
	@echo "KenshiMP v$(VERSION) — available targets:"
	@echo ""
	@echo "  make build         Build core DLL + server core DLL + GUI"
	@echo "  make core          Build only the Kenshi plugin DLL (VS2010 toolset)"
	@echo "  make server-core   Build only the server core DLL"
	@echo "  make server-gui    Build only the Avalonia GUI"
	@echo "  make deploy        Build and copy binaries into Kenshi mods folder"
	@echo "  make publish       Produce dist/KenshiMP-v$(VERSION)-windows-x64.zip"
	@echo "  make run-server    Launch the deployed server GUI"
	@echo "  make clean         Remove build artifacts"
	@echo "  make distclean     Clean + remove dist/"
	@echo ""
	@echo "Override paths on the command line, e.g.:"
	@echo "  make deploy KENSHI_MOD_DIR='D:/SteamLibrary/steamapps/common/Kenshi/mods/KenshiMP'"

all: build

# ---------------------------------------------------------------------------
# Configure steps (regenerate CMake caches when missing)
# ---------------------------------------------------------------------------
$(BUILD_CORE)/CMakeCache.txt:
	$(call BANNER,Configure core/ with VS2010 v100 toolset)
	"$(CMAKE)" -S "$(ROOT)" -B "$(BUILD_CORE)" -G "Visual Studio 17 2022" -A x64 -T v100 \
		-DKENSHIMP_BUILD_CORE=ON -DKENSHIMP_BUILD_SERVER=OFF -DKENSHIMP_BUILD_INJECTOR=OFF \
		-DENET_DIR="$(ENET_DIR)" -DKENSHILIB_DIR="$(KENSHILIB_DIR)" -DBOOST_ROOT="$(BOOST_ROOT)"

$(BUILD_SERVER)/CMakeCache.txt:
	$(call BANNER,Configure server/ with VS2022 v143 toolset)
	"$(CMAKE)" -S "$(ROOT)" -B "$(BUILD_SERVER)" -G "Visual Studio 17 2022" -A x64 -T v143 \
		-DKENSHIMP_BUILD_CORE=OFF -DKENSHIMP_BUILD_SERVER=ON -DKENSHIMP_BUILD_INJECTOR=OFF \
		-DENET_DIR="$(ENET_DIR)"

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------
core: $(BUILD_CORE)/CMakeCache.txt
	$(call BANNER,Build Kenshi plugin DLL (core))
	"$(CMAKE)" --build "$(BUILD_CORE)" --config Release --target KenshiMP

server-core: $(BUILD_SERVER)/CMakeCache.txt
	$(call BANNER,Build server core DLL)
	"$(CMAKE)" --build "$(BUILD_SERVER)" --config Release --target kenshi-mp-server-core

server-gui: server-core
	$(call BANNER,Build Avalonia GUI)
	cd "$(GUI_DIR)" && $(DOTNET) build -c Release -r win-x64 --self-contained

build: core server-gui

# ---------------------------------------------------------------------------
# Deploy — copy into Kenshi mods folder
# ---------------------------------------------------------------------------
deploy: build
	$(call BANNER,Deploying to $(KENSHI_MOD_DIR))
	@mkdir -p "$(KENSHI_MOD_DIR)"
	@# Stop any running server so the exe isn't locked.
	@taskkill //F //IM kenshi-mp-server.exe > /dev/null 2>&1 || true
	@sleep 1 || true
	cp "$(BUILD_CORE)/core/Release/KenshiMP.dll" "$(KENSHI_MOD_DIR)/KenshiMP.dll"
	@# Publish produces a single self-contained exe + the native core DLL.
	cd "$(GUI_DIR)" && $(DOTNET) publish -c Release -r win-x64 --self-contained \
		-p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true \
		-o "$(KENSHI_MOD_DIR)"
	@# RE_Kenshi config + .mod placeholder — only write if missing.
	@test -f "$(KENSHI_MOD_DIR)/RE_Kenshi.json" || \
		printf '{\n    "Plugins": [ "KenshiMP.dll" ]\n}\n' > "$(KENSHI_MOD_DIR)/RE_Kenshi.json"
	@test -f "$(KENSHI_MOD_DIR)/KenshiMP.mod" || \
		: > "$(KENSHI_MOD_DIR)/KenshiMP.mod"
	@echo ""
	@echo "Deployed."
	@ls -la "$(KENSHI_MOD_DIR)"

run-server:
	@echo "Launching $(KENSHI_MOD_DIR)/kenshi-mp-server.exe"
	@cd "$(KENSHI_MOD_DIR)" && ./kenshi-mp-server.exe &

# ---------------------------------------------------------------------------
# Publish — produce a GitHub release zip
# ---------------------------------------------------------------------------
STAGE_DIR := $(DIST_DIR)/KenshiMP-v$(VERSION)
ZIP_FILE  := $(DIST_DIR)/KenshiMP-v$(VERSION)-windows-x64.zip

publish: build
	$(call BANNER,Stage release in $(STAGE_DIR))
	@rm -rf "$(STAGE_DIR)"
	@mkdir -p "$(STAGE_DIR)"

	cp "$(BUILD_CORE)/core/Release/KenshiMP.dll" "$(STAGE_DIR)/KenshiMP.dll"
	cd "$(GUI_DIR)" && $(DOTNET) publish -c Release -r win-x64 --self-contained \
		-p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true \
		-o "$(STAGE_DIR)"

	@# Minimal RE_Kenshi config + .mod placeholder.
	@printf '{\n    "Plugins": [ "KenshiMP.dll" ]\n}\n' > "$(STAGE_DIR)/RE_Kenshi.json"
	@: > "$(STAGE_DIR)/KenshiMP.mod"

	@# Strip debug symbols from the shipping tree.
	@rm -f "$(STAGE_DIR)"/*.pdb "$(STAGE_DIR)"/*.lib "$(STAGE_DIR)"/*.exp || true

	cp "$(ROOT)/dist-assets/README-RELEASE.md" "$(STAGE_DIR)/README.md"

	$(call BANNER,Zipping $(ZIP_FILE))
	@rm -f "$(ZIP_FILE)"
	@cd "$(DIST_DIR)" && powershell -NoProfile -Command \
		"Compress-Archive -Path 'KenshiMP-v$(VERSION)\\*' -DestinationPath 'KenshiMP-v$(VERSION)-windows-x64.zip' -Force"
	@echo ""
	@echo "Release artifact:"
	@ls -la "$(ZIP_FILE)"

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean:
	$(call BANNER,Clean)
	rm -rf "$(BUILD_CORE)" "$(BUILD_SERVER)"
	rm -rf "$(GUI_DIR)/bin" "$(GUI_DIR)/obj"

distclean: clean
	rm -rf "$(DIST_DIR)"
