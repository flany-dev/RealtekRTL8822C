SHELL := /bin/sh

PROJECT := RealtekRTL8822C
VERSION := $(strip $(shell cat VERSION))
CONFIG ?= Debug
ARCH ?= x86_64
MACOS_MIN ?= 15.5
SDK_PATH ?= $(shell xcrun --sdk macosx --show-sdk-path)
MAC_KERNEL_SDK ?= ../MacKernelSDK
LINUX_RTW88 ?= ../rtw88

ifeq ($(filter $(CONFIG),Debug Release),)
$(error CONFIG must be Debug or Release)
endif

BUILD_ROOT := build/$(CONFIG)
OBJ_DIR := $(BUILD_ROOT)/obj
GENERATED_DIR := $(BUILD_ROOT)/generated
KEXT_DIR := $(BUILD_ROOT)/$(PROJECT).kext
KEXT_CONTENTS := $(KEXT_DIR)/Contents
KEXT_BIN_DIR := $(KEXT_CONTENTS)/MacOS
KEXT_BINARY := $(KEXT_BIN_DIR)/$(PROJECT)
DSYM_DIR := $(BUILD_ROOT)/$(PROJECT).dSYM
RTL8822CCTL := $(BUILD_ROOT)/rtl8822cctl
APP_NAME := RealtekRTL8822CMenu
APP_DIR := $(BUILD_ROOT)/$(APP_NAME).app
APP_CONTENTS := $(APP_DIR)/Contents
APP_BIN_DIR := $(APP_CONTENTS)/MacOS
APP_RESOURCES_DIR := $(APP_CONTENTS)/Resources
APP_BINARY := $(APP_BIN_DIR)/$(APP_NAME)
APP_ICON := app/Assets/AppIcon.icns
APP_BRIDGE_OBJECT := $(OBJ_DIR)/RTL8822CClient.o
FIRMWARE := firmware/rtw8822c_fw.bin
FIRMWARE_HEADER := $(GENERATED_DIR)/rtw8822c_fw.h
FIRMWARE_SHA256 := 3deecb31210986d98cdbfb000391e08d602a6eee4ffc883969faa2b907ab03ba
FIRMWARE_LICENCE := firmware/LICENCE.rtlwifi_firmware.txt
FIRMWARE_LICENCE_SHA256 := a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e

CXX := clang++
CC := clang
PYTHON ?= python3

KERNEL_DEFINES := -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-D__IO80211_TARGET=140400 -DIO80211FAMILY_V2 -D__PRIVATE_SPI__ -DAIRPORT
KERNEL_INCLUDES := -I$(GENERATED_DIR) -Iinclude -Ifirmware \
	-isystem $(MAC_KERNEL_SDK)/Headers \
	-isystem $(MAC_KERNEL_SDK)/Headers/Kernel \
	-isystem $(MAC_KERNEL_SDK)/Headers/Kernel/i386
KERNEL_COMMON := -target $(ARCH)-apple-macos$(MACOS_MIN) -mkernel -fno-builtin \
	-ffreestanding -Wall -Wextra -Wpedantic -Werror -Wno-extra-semi \
	-Wno-zero-length-array -MMD -MP $(KERNEL_DEFINES) \
	$(KERNEL_INCLUDES) -isysroot $(SDK_PATH)
CXX_COMMON := $(KERNEL_COMMON) -x c++ -fno-rtti -fno-exceptions -std=c++14
C_COMMON := $(KERNEL_COMMON) -x c

ifeq ($(CONFIG),Debug)
KERNEL_PROFILE_FLAGS := -O0 -g -DRTW_DEBUG=1
TOOL_PROFILE_FLAGS := -O0 -g -DRTW_DEBUG=1
APP_PROFILE_FLAGS := -Onone -g -D RTW_APP_DEBUG
APP_BRIDGE_DEFINES := -DRTW_APP_DEBUG=1
else
KERNEL_PROFILE_FLAGS := -O2 -g -DNDEBUG -DRTW_DEBUG=0
TOOL_PROFILE_FLAGS := -O2 -DNDEBUG -DRTW_DEBUG=0
APP_PROFILE_FLAGS := -O
APP_BRIDGE_DEFINES := -DRTW_APP_DEBUG=0
endif

CXXFLAGS := $(CXX_COMMON) $(KERNEL_PROFILE_FLAGS) -DRTW_VERSION=\"$(VERSION)\"
CFLAGS := $(C_COMMON) $(KERNEL_PROFILE_FLAGS) -DRTW_VERSION=\"$(VERSION)\"
LDFLAGS := -target $(ARCH)-apple-macos$(MACOS_MIN) -Xlinker -kext -nostdlib \
	-lkmodc++ -lkmod -lcc_kext -isysroot $(SDK_PATH)

DRIVER_OBJECTS := $(OBJ_DIR)/RealtekRTL8822C.o $(OBJ_DIR)/RtwWpaCrypto.o \
	$(OBJ_DIR)/RealtekRTL8822C_info.o
DEPENDENCIES := $(DRIVER_OBJECTS:.o=.d)

.PHONY: all debug release tools app symbols check privacy-check format-check dma-cache-policy-check release-surface-check profile-check tables-check \
	test host-sanitizers reproducibility-check release-check package package-local \
	package-internal package-kexts package-app verify-publication clean distclean verify-env

all: verify-env $(KEXT_BINARY)

debug:
	$(MAKE) CONFIG=Debug all tools app

release:
	$(MAKE) CONFIG=Release all tools app symbols

verify-env:
	@test -f VERSION || { echo "missing VERSION"; exit 1; }
	@test -d "$(MAC_KERNEL_SDK)/Headers" || { \
		echo "MacKernelSDK not found at $(MAC_KERNEL_SDK)"; exit 1; }
	@test -d "$(SDK_PATH)" || { echo "macOS SDK not found at $(SDK_PATH)"; exit 1; }

$(FIRMWARE_HEADER): $(FIRMWARE) scripts/embed_firmware.py
	@mkdir -p $(GENERATED_DIR)
	$(PYTHON) scripts/embed_firmware.py $(FIRMWARE) $@ --sha256 $(FIRMWARE_SHA256)

$(OBJ_DIR)/RealtekRTL8822C.o: src/RealtekRTL8822C.cpp $(FIRMWARE_HEADER)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/RtwWpaCrypto.o: src/RtwWpaCrypto.cpp include/RtwWpaCrypto.hpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/RealtekRTL8822C_info.o: src/RealtekRTL8822C_info.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT_BINARY): $(DRIVER_OBJECTS) Info.plist VERSION
	@mkdir -p $(KEXT_BIN_DIR)
	sed 's/@VERSION@/$(VERSION)/g' Info.plist > $(KEXT_CONTENTS)/Info.plist
	$(CXX) $(LDFLAGS) $(DRIVER_OBJECTS) -o $@
	chmod -R 755 $(KEXT_DIR)

symbols: $(DSYM_DIR)

$(DSYM_DIR): $(KEXT_BINARY)
	dsymutil $(KEXT_BINARY) -o $@
	strip -S $(KEXT_BINARY)
	@touch $@

tools: $(RTL8822CCTL)

app: $(APP_BINARY)

$(RTL8822CCTL): tools/rtl8822cctl/main.cpp $(APP_BRIDGE_OBJECT) \
		app/bridge/RTL8822CClient.h include/RTL8822CUserClientShared.h VERSION
	@mkdir -p $(BUILD_ROOT)
	$(CXX) -target $(ARCH)-apple-macos$(MACOS_MIN) -arch $(ARCH) \
		-isysroot $(SDK_PATH) -std=c++14 -Wall -Wextra -Wpedantic -Werror $(TOOL_PROFILE_FLAGS) \
		-Iinclude -Iapp/bridge -DRTW_VERSION=\"$(VERSION)\" $< \
		$(APP_BRIDGE_OBJECT) \
		-framework CoreFoundation -framework IOKit -o $@

$(APP_BRIDGE_OBJECT): app/bridge/RTL8822CClient.c \
		app/bridge/RTL8822CClient.h include/RTL8822CUserClientShared.h
	@mkdir -p $(OBJ_DIR)
	$(CC) -target $(ARCH)-apple-macos$(MACOS_MIN) -arch $(ARCH) \
		-isysroot $(SDK_PATH) -Iinclude -Iapp/bridge \
		$(APP_BRIDGE_DEFINES) -Wall -Wextra -Wpedantic -Werror -c $< -o $@

$(APP_BINARY): app/RealtekRTL8822CMenu.swift app/Info.plist $(APP_ICON) \
		$(APP_BRIDGE_OBJECT) VERSION
	@mkdir -p $(APP_BIN_DIR) $(APP_RESOURCES_DIR)
	sed 's/@VERSION@/$(VERSION)/g' app/Info.plist > $(APP_CONTENTS)/Info.plist
	cp $(APP_ICON) $(APP_RESOURCES_DIR)/AppIcon.icns
	xcrun swiftc -parse-as-library -target $(ARCH)-apple-macos$(MACOS_MIN) -sdk $(SDK_PATH) \
		$(APP_PROFILE_FLAGS) $< $(APP_BRIDGE_OBJECT) \
		-framework AppKit -framework Foundation -framework Security \
		-framework ServiceManagement \
		-framework CoreFoundation -framework IOKit -o $@
	chmod -R 755 $(APP_DIR)
	codesign --force --sign - --timestamp=none $(APP_DIR)

check: all tools app
	plutil -lint $(KEXT_CONTENTS)/Info.plist
	plutil -lint $(APP_CONTENTS)/Info.plist
	@test "`/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $(KEXT_CONTENTS)/Info.plist`" = "$(VERSION)"
	@test "`/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $(APP_CONTENTS)/Info.plist`" = "$(VERSION)"
	@shasum -a 256 $(FIRMWARE) | grep -q '^$(FIRMWARE_SHA256) '
	@shasum -a 256 $(FIRMWARE_LICENCE) | grep -q '^$(FIRMWARE_LICENCE_SHA256) '
	$(MAKE) privacy-check

privacy-check:
	sh scripts/check_documentation_privacy.sh

format-check:
	sh scripts/check_formatting.sh

dma-cache-policy-check:
	sh scripts/check_dma_cache_policy.sh

release-surface-check:
	sh scripts/check_release_surface.sh

profile-check: debug release
	sh scripts/check_profile_separation.sh

tables-check:
	@mkdir -p build/Verification
	$(PYTHON) scripts/generate_rtl8822c_tables.py \
		$(LINUX_RTW88)/rtw8822c_table.c \
		build/Verification/rtw8822c_tables.h
	cmp firmware/rtw8822c_tables.h build/Verification/rtw8822c_tables.h

test: debug release privacy-check format-check dma-cache-policy-check
	plutil -lint build/Debug/$(PROJECT).kext/Contents/Info.plist
	plutil -lint build/Release/$(PROJECT).kext/Contents/Info.plist
	@test "`/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' build/Debug/$(PROJECT).kext/Contents/Info.plist`" = "$(VERSION)"
	@test "`/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' build/Release/$(PROJECT).kext/Contents/Info.plist`" = "$(VERSION)"
	@shasum -a 256 $(FIRMWARE) | grep -q '^$(FIRMWARE_SHA256) '
	@shasum -a 256 $(FIRMWARE_LICENCE) | grep -q '^$(FIRMWARE_LICENCE_SHA256) '
	@mkdir -p build/Tests
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/reorder_model.cpp -o build/Tests/reorder_model
	build/Tests/reorder_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/bandwidth_model.cpp -o build/Tests/bandwidth_model
	build/Tests/bandwidth_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/wmm_edca_model.cpp -o build/Tests/wmm_edca_model
	build/Tests/wmm_edca_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/connected_scan_model.cpp -o build/Tests/connected_scan_model
	build/Tests/connected_scan_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/lifecycle_model.cpp -o build/Tests/lifecycle_model
	build/Tests/lifecycle_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/hotpath_diagnostics_model.cpp -o build/Tests/hotpath_diagnostics_model
	build/Tests/hotpath_diagnostics_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/rx_poll_model.cpp -o build/Tests/rx_poll_model
	build/Tests/rx_poll_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/rx_delivery_model.cpp -o build/Tests/rx_delivery_model
	build/Tests/rx_delivery_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		tests/protocol_validation_model.cpp -o build/Tests/protocol_validation_model
	build/Tests/protocol_validation_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror -Iinclude \
		tests/user_client_model.cpp -o build/Tests/user_client_model
	build/Tests/user_client_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror -DRTW_WPA_USERSPACE \
		-Iinclude src/RtwWpaCrypto.cpp tests/wpa_crypto_test.cpp \
		-o build/Tests/wpa_crypto
	build/Tests/wpa_crypto
	@build/Debug/rtl8822cctl --version | grep -q '^rtl8822cctl $(VERSION)$$'
	@build/Debug/rtl8822cctl --help | grep -q '^Usage:'
	@build/Debug/rtl8822cctl invalid-command >/dev/null 2>&1; \
		status=$$?; test $$status -eq 64
	$(MAKE) release-surface-check
	$(MAKE) profile-check

host-sanitizers:
	@mkdir -p build/Sanitizers
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/protocol_validation_model.cpp -o build/Sanitizers/protocol_validation_model
	ASAN_OPTIONS=detect_leaks=0 build/Sanitizers/protocol_validation_model
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic -Werror \
		-fsanitize=address,undefined -fno-omit-frame-pointer -DRTW_WPA_USERSPACE \
		-Iinclude src/RtwWpaCrypto.cpp tests/wpa_crypto_test.cpp \
		-o build/Sanitizers/wpa_crypto
	ASAN_OPTIONS=detect_leaks=0 build/Sanitizers/wpa_crypto

reproducibility-check:
	sh scripts/check_release_reproducibility.sh

release-check: test host-sanitizers tables-check reproducibility-check package
	sh scripts/check_package_reproducibility.sh
	$(MAKE) debug release

verify-publication: release-surface-check
	@shasum -a 256 $(FIRMWARE) | grep -q '^$(FIRMWARE_SHA256) ' || { \
		echo "public packaging blocked: unexpected firmware input"; exit 1; \
	}
	@shasum -a 256 $(FIRMWARE_LICENCE) | grep -q '^$(FIRMWARE_LICENCE_SHA256) ' || { \
		echo "public packaging blocked: missing or modified firmware licence"; exit 1; \
	}

package-local:
	$(MAKE) CONFIG=Release PACKAGE_SUFFIX=-local package-internal

package: verify-publication
	$(MAKE) CONFIG=Release PACKAGE_SUFFIX= package-internal
	$(MAKE) package-kexts
	$(MAKE) package-app

package-kexts: debug release
	$(PYTHON) scripts/create_release_archive.py \
		build/Release/$(PROJECT).kext \
		build/package/$(PROJECT)-$(VERSION)-Release.zip
	$(PYTHON) scripts/create_release_archive.py \
		build/Debug/$(PROJECT).kext \
		build/package/$(PROJECT)-$(VERSION)-Debug.zip

package-app: release
	$(PYTHON) scripts/create_release_archive.py \
		build/Release/$(APP_NAME).app \
		build/package/$(APP_NAME)-$(VERSION).zip

package-internal: check symbols
	@rm -rf build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)
	@mkdir -p build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)
	ditto $(KEXT_DIR) build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/$(PROJECT).kext
	ditto $(RTL8822CCTL) build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/rtl8822cctl
	ditto $(APP_DIR) build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/$(APP_NAME).app
	@if [ -d "$(DSYM_DIR)" ]; then \
		mkdir -p build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/Symbols; \
		ditto $(DSYM_DIR) \
			build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/Symbols/$(PROJECT).dSYM; \
	fi
	ditto README.md RELEASE_NOTES.md LICENSE THIRD_PARTY_NOTICES.md CHANGELOG.md SECURITY.md \
		CODE_OF_CONDUCT.md CONTRIBUTING.md ROADMAP.md \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/
	@mkdir -p build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/docs
	ditto docs/RTL8822CCTL.md \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/docs/RTL8822CCTL.md
	ditto docs/MENU_APP.md \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/docs/MENU_APP.md
	ditto docs/HARDWARE_ACCEPTANCE.md \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/docs/HARDWARE_ACCEPTANCE.md
	@mkdir -p build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/firmware
	ditto firmware/README.md \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/firmware/README.md
	ditto $(FIRMWARE_LICENCE) \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/firmware/LICENCE.rtlwifi_firmware.txt
	sh scripts/create_manifest.sh $(VERSION) $(KEXT_DIR) $(RTL8822CCTL) $(APP_DIR) \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX)/MANIFEST.txt
	$(PYTHON) scripts/create_release_archive.py \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX) \
		build/package/$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX).zip
	cd build/package && shasum -a 256 \
		$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX).zip > \
		$(PROJECT)-$(VERSION)$(PACKAGE_SUFFIX).sha256

clean:
	rm -rf build/Debug build/Release build/Tests build/Sanitizers \
		build/Verification build/package

distclean: clean
	rm -rf build

-include $(DEPENDENCIES)
