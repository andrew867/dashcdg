CC ?= gcc
AR ?= ar

BUILD_DIR := build

COMMON_CFLAGS := -Wall -Wextra -Wno-cpp -std=c99 -pedantic -D_FORTIFY_SOURCE=2
INCLUDES := -Icore/include -Iproto/include -Iplatform/desktop/include -Iinc
EXTRA_LDFLAGS :=
CFLAGS ?= $(COMMON_CFLAGS) $(INCLUDES)
UNAME_S := $(shell uname -s 2>/dev/null)

ifneq (,$(filter Windows_NT MINGW64_NT% MINGW32_NT% MSYS_NT%,$(OS) $(UNAME_S)))
MINGW_ARCH ?= mingw64
ifeq ($(MINGW_ARCH),mingw32)
WINDOWS_ARCH_LABEL := x86
WINDOWS_BUILD_SLUG := x86
else
WINDOWS_ARCH_LABEL := x64
WINDOWS_BUILD_SLUG := amd64
endif
BUILD_DIR := build/$(WINDOWS_BUILD_SLUG)
WINDOWS_RETRO_BUNDLE ?= 0
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
ifneq ($(MINGW_ARCH),mingw32)
$(error WINDOWS_RETRO_BUNDLE=1 requires MINGW_ARCH=mingw32)
endif
override BUILD_DIR := build/x86-retro
WINDOWS_ARCH_LABEL := x86-retro
WINDOWS_BUILD_SLUG := x86-retro
WINDOWS_LEGACY_WINNT_RETRO := 0x0500
CFLAGS += -D_WIN32_WINNT=$(WINDOWS_LEGACY_WINNT_RETRO) -DWINVER=$(WINDOWS_LEGACY_WINNT_RETRO)
EXTRA_LDFLAGS += -Wl,--major-os-version=5 -Wl,--minor-os-version=0 -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=0
endif
WINDOWS_MINGW_PREFIX := $(shell if [ -d /c/msys64/$(MINGW_ARCH) ]; then echo /c/msys64/$(MINGW_ARCH); elif [ -d /c/ProgramData/mingw64/$(MINGW_ARCH) ]; then echo /c/ProgramData/mingw64/$(MINGW_ARCH); elif [ -d /$(MINGW_ARCH) ]; then echo /$(MINGW_ARCH); fi)
WINDOWS_MINGW_PREFIX_WIN := $(shell if [ -n "$(WINDOWS_MINGW_PREFIX)" ]; then cygpath -m "$(WINDOWS_MINGW_PREFIX)"; fi)
ifneq ($(WINDOWS_MINGW_PREFIX),)
export PATH := $(WINDOWS_MINGW_PREFIX)/bin:$(PATH)
# Use the prefix’s driver explicitly so MINGW_ARCH=mingw32 never picks up mingw64’s gcc
# from PATH (would reject -march=pentium3: “does not support x86-64 instruction set”).
override CC := $(WINDOWS_MINGW_PREFIX)/bin/gcc
override AR := $(WINDOWS_MINGW_PREFIX)/bin/ar
INCLUDES += -I$(WINDOWS_MINGW_PREFIX_WIN)/include
EXTRA_LDFLAGS += -L$(WINDOWS_MINGW_PREFIX_WIN)/lib
WINDOWS_LEGACY_TARGET ?= 0
ifeq ($(WINDOWS_LEGACY_TARGET),1)
ifeq ($(WINDOWS_RETRO_BUNDLE),0)
WINDOWS_LEGACY_WINNT := 0x0501
CFLAGS += -D_WIN32_WINNT=$(WINDOWS_LEGACY_WINNT) -DWINVER=$(WINDOWS_LEGACY_WINNT)
EXTRA_LDFLAGS += -Wl,--major-os-version=5 -Wl,--minor-os-version=1 -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1
endif
# 64-bit PE only: i686 GNU ld rejects --disable-high-entropy-va (breaks mingw32 link).
ifneq ($(MINGW_ARCH),mingw32)
ifeq ($(WINDOWS_RETRO_BUNDLE),0)
EXTRA_LDFLAGS += -Wl,--disable-high-entropy-va
endif
endif
endif
# Pre-SSE2 CPUs (e.g. Pentium III): MSYS2 i686 defaults often define __SSE2__; minimp3 then
# emits SSE2 intrinsics. Force scalar FP + no SSE2 for legacy and retro Win32 bundles.
ifeq ($(MINGW_ARCH),mingw32)
WINDOWS_CPU_PRE_SSE2 := 0
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
WINDOWS_CPU_PRE_SSE2 := 1
endif
ifeq ($(WINDOWS_LEGACY_TARGET),1)
ifeq ($(WINDOWS_RETRO_BUNDLE),0)
WINDOWS_CPU_PRE_SSE2 := 1
endif
endif
ifeq ($(WINDOWS_CPU_PRE_SSE2),1)
# Drop fortify/stdio inlines and tree vectorization; both can still emit SSE2 insns in .text
# even when -mno-sse2 is set (seen as illegal instruction on Pentium III).
CFLAGS += -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -DDASHCDG_CPU_PRE_SSE2_MINIMP3=1
CFLAGS += -U_FORTIFY_SOURCE -fno-tree-vectorize -fno-tree-slp-vectorize
# MinGW's ANSI stdio pulls in mingw_pformat.c (uses SSE2); use MSVCRT printf path on XP-era CPUs.
CFLAGS += -D__USE_MINGW_ANSI_STDIO=0
# MSVCRT printf checking disagrees with %zu under -pedantic; silence format warnings for this column.
CFLAGS += -Wno-format
endif
endif
WINDOWS_RUNTIME_DLLS := $(shell for pattern in libfreeglut.dll glew32.dll libportaudio.dll libwinpthread-1.dll libopus-0.dll libgcc_s_*.dll libstdc++-6.dll; do for f in "$(WINDOWS_MINGW_PREFIX)"/bin/$$pattern; do if [ -f "$$f" ]; then printf '%s ' "$$f"; fi; done; done)
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
WINDOWS_RUNTIME_DLLS := $(shell for pattern in libportaudio.dll libwinpthread-1.dll libgcc_s_*.dll libstdc++-6.dll; do for f in "$(WINDOWS_MINGW_PREFIX)"/bin/$$pattern; do if [ -f "$$f" ]; then printf '%s ' "$$f"; fi; done; done)
endif
else
WINDOWS_RUNTIME_DLLS :=
endif
LDLIBS_DESKTOP := -lopengl32 -lglew32 -lfreeglut -lportaudio -lopus -lpthread
NET_LIBS := -lws2_32 -liphlpapi
WINDOWS_GDI_LIBS := -lgdi32 -luser32
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
LDLIBS_DESKTOP_RETRO := -lportaudio -lpthread $(NET_LIBS) $(WINDOWS_GDI_LIBS)
endif
else
LDLIBS_DESKTOP := -lGL -lGLEW -lglut -lportaudio -lopus -lpthread
NET_LIBS :=
WINDOWS_GDI_LIBS :=
WINDOWS_RUNTIME_DLLS :=
WINDOWS_ARCH_LABEL ?= x64
LDLIBS_DESKTOP_RETRO :=
endif

OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

ifneq (,$(filter Windows_NT MINGW64_NT% MINGW32_NT% MSYS_NT%,$(OS) $(UNAME_S)))
RX_GDI_BIN := $(BIN_DIR)/desktop-gdi-rx.exe
TX_GDI_BIN := $(BIN_DIR)/desktop-gdi-tx.exe
LDLIBS_DESKTOP_RX_GDI := -lportaudio -lopus -lpthread $(NET_LIBS) $(WINDOWS_GDI_LIBS)
LDLIBS_DESKTOP_TX_GDI := -lportaudio -lopus -lpthread $(NET_LIBS) $(WINDOWS_GDI_LIBS)
RETRO_RX_BIN :=
RETRO_TX_BIN :=
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
RETRO_RX_BIN := $(BIN_DIR)/desktop-retro-rx.exe
RETRO_TX_BIN := $(BIN_DIR)/desktop-retro-tx.exe
endif
else
RX_GDI_BIN :=
TX_GDI_BIN :=
LDLIBS_DESKTOP_RX_GDI :=
LDLIBS_DESKTOP_TX_GDI :=
RETRO_RX_BIN :=
RETRO_TX_BIN :=
endif
LIB_DIR := $(BUILD_DIR)/lib
RELEASE_DIR := $(BUILD_DIR)/release
WINDOWS_DIST_DIR := build/dist
WINDOWS_PACKAGE_ZIP = $(RELEASE_DIR)/dashcdg-windows-$(WINDOWS_ARCH_LABEL)-portable.zip
WINDOWS_ZIP_X64 := build/amd64/release/dashcdg-windows-x64-portable.zip
WINDOWS_ZIP_X86 := build/x86/release/dashcdg-windows-x86-portable.zip

CORE_SOURCES := core/src/cdg.c core/src/media_clock.c core/src/cdg_raster.c core/src/audio_jitter.c core/src/cdg_batch_jitter.c core/src/nb_ima_codec.c
PROTO_SOURCES := proto/src/protocol.c proto/src/fec.c
CORE_OBJECTS := $(OBJ_DIR)/core_cdg.o $(OBJ_DIR)/core_media_clock.o $(OBJ_DIR)/core_cdg_raster.o $(OBJ_DIR)/core_audio_jitter.o $(OBJ_DIR)/core_cdg_batch_jitter.o $(OBJ_DIR)/core_nb_ima_codec.o
PROTO_OBJECTS := $(OBJ_DIR)/proto_protocol.o $(OBJ_DIR)/proto_fec.o
DESKTOP_COMMON_OBJECTS := $(OBJ_DIR)/desktop_file_io.o $(OBJ_DIR)/desktop_net_compat.o
DESKTOP_LIB_OBJECTS := $(OBJ_DIR)/desktop_audio.o $(OBJ_DIR)/desktop_cdg_source.o $(OBJ_DIR)/desktop_gl_renderer.o $(OBJ_DIR)/desktop_stream_runtime.o $(OBJ_DIR)/desktop_transport_udp.o $(OBJ_DIR)/desktop_win32_gdi_view.o
DESKTOP_OPUS_OBJECT := $(OBJ_DIR)/desktop_opus_codec.o
DESKTOP_OPUS_STUB_OBJECT := $(OBJ_DIR)/desktop_opus_codec_stub.o
DESKTOP_TX_OBJECT := $(OBJ_DIR)/desktop_app_tx.o
DESKTOP_TX_HEADLESS_OBJECT := $(OBJ_DIR)/desktop_app_tx_headless.o
DESKTOP_TX_GDI_OBJECT := $(OBJ_DIR)/desktop_app_tx_gdi.o
DESKTOP_TX_RETRO_OBJECT := $(OBJ_DIR)/desktop_app_tx_retro.o
DESKTOP_RX_GL_OBJECT := $(OBJ_DIR)/desktop_app_rx.o
DESKTOP_RX_GDI_OBJECT := $(OBJ_DIR)/desktop_app_rx_gdi.o
DESKTOP_RX_RETRO_GDI_OBJECT := $(OBJ_DIR)/desktop_app_rx_retro_gdi.o

CORE_LIB := $(LIB_DIR)/libdashcdg_core.a
PROTO_LIB := $(LIB_DIR)/libdashcdg_proto.a
DESKTOP_LIB := $(LIB_DIR)/libdashcdg_desktop.a

TEST_BIN := $(BIN_DIR)/test-core
TEST_TRANSPORT_UDP_BIN := $(BIN_DIR)/test-transport-udp
PLAYER_BIN := $(BIN_DIR)/desktop-player
TX_BIN := $(BIN_DIR)/desktop-tx
RX_BIN := $(BIN_DIR)/desktop-rx

DESKTOP_RX_GDI_TARGET :=
ifneq ($(RX_GDI_BIN),)
DESKTOP_RX_GDI_TARGET := $(RX_GDI_BIN)
endif

DESKTOP_TX_GDI_TARGET :=
ifneq ($(TX_GDI_BIN),)
DESKTOP_TX_GDI_TARGET := $(TX_GDI_BIN)
endif

DESKTOP_TX_LINK_OBJ :=
ifneq (,$(filter Windows_NT MINGW64_NT% MINGW32_NT% MSYS_NT%,$(OS) $(UNAME_S)))
DESKTOP_TX_LINK_OBJ := $(DESKTOP_TX_HEADLESS_OBJECT)
else
DESKTOP_TX_LINK_OBJ := $(DESKTOP_TX_OBJECT)
endif

DESKTOP_RETRO_BINS :=
ifneq ($(RETRO_RX_BIN),)
DESKTOP_RETRO_BINS := $(RETRO_RX_BIN) $(RETRO_TX_BIN)
endif

.PHONY: all debug dirs libs test desktop-apps bundle-runtime package package-x64 package-x86 package-all-windows dist-windows dist-windows-sneakernet desktop-windows-x86-retro release clean

all: CFLAGS += -O2
all: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(TEST_BIN) $(TX_BIN)

debug: CFLAGS += -DDEBUG -g
debug: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(TEST_BIN) $(TX_BIN) $(PLAYER_BIN) $(RX_BIN) $(DESKTOP_RX_GDI_TARGET) $(DESKTOP_TX_GDI_TARGET) $(DESKTOP_RETRO_BINS)

desktop-apps: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(PLAYER_BIN) $(TX_BIN) $(RX_BIN) $(DESKTOP_RX_GDI_TARGET) $(DESKTOP_TX_GDI_TARGET) $(DESKTOP_RETRO_BINS)

desktop-windows-x86-retro:
	$(MAKE) clean debug MINGW_ARCH=mingw32 WINDOWS_RETRO_BUNDLE=1

dirs:
	mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR) $(RELEASE_DIR)

bundle-runtime:
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

libs: $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB)

$(CORE_LIB): $(CORE_OBJECTS)
	$(AR) rcs $@ $^

$(PROTO_LIB): $(PROTO_OBJECTS)
	$(AR) rcs $@ $^

$(DESKTOP_LIB): $(DESKTOP_LIB_OBJECTS)
	$(AR) rcs $@ $^

$(OBJ_DIR)/core_cdg.o: core/src/cdg.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/core_media_clock.o: core/src/media_clock.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/core_cdg_raster.o: core/src/cdg_raster.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/core_audio_jitter.o: core/src/audio_jitter.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/core_cdg_batch_jitter.o: core/src/cdg_batch_jitter.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/core_nb_ima_codec.o: core/src/nb_ima_codec.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/proto_protocol.o: proto/src/protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/proto_fec.o: proto/src/fec.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_file_io.o: platform/desktop/src/file_io.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_net_compat.o: platform/desktop/src/net_compat.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_audio.o: platform/desktop/src/desktop_audio.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_opus_codec.o: platform/desktop/src/opus_codec.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_opus_codec_stub.o: platform/desktop/src/opus_codec.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_NO_OPUS=1 -c -o $@ $<

$(OBJ_DIR)/desktop_cdg_source.o: platform/desktop/src/cdg_source.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_gl_renderer.o: platform/desktop/src/gl_renderer.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_stream_runtime.o: platform/desktop/src/stream_runtime.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_transport_udp.o: platform/desktop/src/transport_udp.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_win32_gdi_view.o: platform/desktop/src/win32_gdi_view.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_tx.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_tx_headless.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_TX_HEADLESS=1 -c -o $@ $<

ifneq ($(TX_GDI_BIN),)
$(OBJ_DIR)/desktop_app_tx_gdi.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_TX_GDI_PREVIEW=1 -c -o $@ $<
endif

$(OBJ_DIR)/desktop_app_tx_retro.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_RETRO_WINDOWS=1 -DDASHCDG_DESKTOP_NO_OPUS=1 -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx_gdi.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -DDASHCDG_RX_UI_GDI_ONLY=1 -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx_retro_gdi.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -DDASHCDG_RX_UI_GDI_ONLY=1 -DDASHCDG_DESKTOP_NO_OPUS=1 -c -o $@ $<

$(OBJ_DIR)/test_core.o: tests/test_core.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/test_transport_udp.o: tests/test_transport_udp.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/app_desktop_player.o: apps/desktop-player/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/app_desktop_tx.o: apps/desktop-tx/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/app_desktop_rx.o: apps/desktop-rx/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(OBJ_DIR)/test_core.o $(CORE_LIB) $(PROTO_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/test_core.o $(CORE_LIB) $(PROTO_LIB)

$(TEST_TRANSPORT_UDP_BIN): $(OBJ_DIR)/test_transport_udp.o $(OBJ_DIR)/desktop_transport_udp.o $(OBJ_DIR)/desktop_net_compat.o
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/test_transport_udp.o $(OBJ_DIR)/desktop_transport_udp.o $(OBJ_DIR)/desktop_net_compat.o $(NET_LIBS)

$(PLAYER_BIN): $(OBJ_DIR)/app_desktop_player.o $(DESKTOP_RX_GL_OBJECT) $(DESKTOP_TX_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_player.o $(DESKTOP_RX_GL_OBJECT) $(DESKTOP_TX_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS) $(WINDOWS_GDI_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(TX_BIN): $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_LINK_OBJ) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
ifeq (,$(filter Windows_NT MINGW64_NT% MINGW32_NT% MSYS_NT%,$(OS) $(UNAME_S)))
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_LINK_OBJ) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS)
else
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_LINK_OBJ) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_RX_GDI)
endif
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(RX_BIN): $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_GL_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_GL_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS) $(WINDOWS_GDI_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

ifneq ($(RX_GDI_BIN),)
$(RX_GDI_BIN): $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_GDI_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_GDI_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_RX_GDI)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif
endif

ifneq ($(TX_GDI_BIN),)
$(TX_GDI_BIN): $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_GDI_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_GDI_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_TX_GDI)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif
endif

ifneq ($(RETRO_RX_BIN),)
$(RETRO_RX_BIN): $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_RETRO_GDI_OBJECT) $(DESKTOP_OPUS_STUB_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_RETRO_GDI_OBJECT) $(DESKTOP_OPUS_STUB_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_RETRO)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(RETRO_TX_BIN): $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_RETRO_OBJECT) $(DESKTOP_OPUS_STUB_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_RETRO_OBJECT) $(DESKTOP_OPUS_STUB_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_RETRO)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif
endif

test: dirs $(CORE_LIB) $(PROTO_LIB) $(TEST_BIN) $(TEST_TRANSPORT_UDP_BIN)
	$(TEST_BIN)
	$(TEST_TRANSPORT_UDP_BIN)

package: debug
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	rm -f "$(WINDOWS_PACKAGE_ZIP)"
	powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '$(subst /,\,$(BIN_DIR))\*' -DestinationPath '$(subst /,\,$(WINDOWS_PACKAGE_ZIP))' -Force"
else
	@echo "package target currently supports Windows/MSYS2 desktop bundles only" && false
endif

package-x64:
	$(MAKE) clean package MINGW_ARCH=mingw64

package-x86:
	$(MAKE) clean package MINGW_ARCH=mingw32

package-all-windows: package-x64 package-x86

dist-windows: package-all-windows
	mkdir -p $(WINDOWS_DIST_DIR)
	cp -f $(WINDOWS_ZIP_X64) $(WINDOWS_ZIP_X86) $(WINDOWS_DIST_DIR)/

dist-windows-sneakernet:
	bash scripts/build_windows_sneakernet_dist.sh

release: package

clean:
	rm -rf $(BUILD_DIR)
