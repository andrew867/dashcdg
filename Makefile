CC ?= gcc
AR ?= ar

BUILD_DIR := build

COMMON_CFLAGS := -Wall -Wextra -Wno-cpp -std=c99 -pedantic -D_FORTIFY_SOURCE=2
INCLUDES := -Icore/include -Iproto/include -Iplatform/desktop/include -Iinc
EXTRA_LDFLAGS :=
CFLAGS ?= $(COMMON_CFLAGS) $(INCLUDES)
UNAME_S := $(shell uname -s 2>/dev/null)

# Opus / PortAudio link flags (expanded after MINGW_ARCH is known on Windows). Linux: set DASHCDG_OPUS_VENDOR=1 and prefixes if needed.
OPUS_VENDOR_PREFIX ?=
PORTAUDIO_VENDOR_PREFIX ?=
OPUS_CPPFLAGS :=
OPUS_LINK := -lopus
PORTAUDIO_CPPFLAGS :=
DESKTOP_PORTAUDIO_LINK := -lportaudio

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
# MinGW i686: default vendored PIII-safe shared Opus + PortAudio (scripts/build_mingw32_p3_opus_portaudio_shared.sh). Set DASHCDG_OPUS_VENDOR=0 to use MSYS2 DLLs.
ifeq ($(MINGW_ARCH),mingw32)
OPUS_VENDOR_PREFIX ?= build/mingw32-p3-vendor/opus
PORTAUDIO_VENDOR_PREFIX ?= build/mingw32-p3-vendor/portaudio
DASHCDG_OPUS_VENDOR ?= 1
DASHCDG_PORTAUDIO_VENDOR ?= 1
endif
ifeq ($(DASHCDG_OPUS_VENDOR),1)
ifneq ($(OPUS_VENDOR_PREFIX),)
ifneq ($(wildcard $(OPUS_VENDOR_PREFIX)/lib/libopus.dll.a),)
OPUS_CPPFLAGS := -I$(OPUS_VENDOR_PREFIX)/include -DDASHCDG_OPUS_VENDOR_BUILD=1
OPUS_LINK := -L$(OPUS_VENDOR_PREFIX)/lib -lopus
endif
endif
endif
ifeq ($(DASHCDG_PORTAUDIO_VENDOR),1)
ifneq ($(PORTAUDIO_VENDOR_PREFIX),)
ifneq ($(wildcard $(PORTAUDIO_VENDOR_PREFIX)/lib/libportaudio.dll.a),)
PORTAUDIO_CPPFLAGS := -I$(PORTAUDIO_VENDOR_PREFIX)/include -DDASHCDG_PORTAUDIO_VENDOR_BUILD=1
DESKTOP_PORTAUDIO_LINK := -L$(PORTAUDIO_VENDOR_PREFIX)/lib -lportaudio
endif
endif
endif
CFLAGS += $(OPUS_CPPFLAGS) $(PORTAUDIO_CPPFLAGS)
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
# MSVCRT on XP does not implement C99 %zu / %ll* reliably; desktop code uses I64u/I64d-style HUD prints.
# MSVCRT printf checking disagrees with %zu under -pedantic; silence format warnings for this column.
CFLAGS += -Wno-format
endif
endif
# Opus/PortAudio: prefer scripts/build_mingw32_p3_opus_portaudio_shared.sh outputs when present.
WINDOWS_OPUS_PORTAUDIO_DLLS :=
ifeq ($(MINGW_ARCH),mingw32)
ifneq ($(wildcard $(OPUS_VENDOR_PREFIX)/bin/libopus-0.dll),)
WINDOWS_OPUS_PORTAUDIO_DLLS := $(OPUS_VENDOR_PREFIX)/bin/libopus-0.dll $(PORTAUDIO_VENDOR_PREFIX)/bin/libportaudio.dll
else
WINDOWS_OPUS_PORTAUDIO_DLLS := $(shell for pattern in libportaudio.dll libopus-0.dll; do for f in "$(WINDOWS_MINGW_PREFIX)"/bin/$$pattern; do if [ -f "$$f" ]; then printf '%s ' "$$f"; fi; done; done)
endif
endif
ifeq ($(MINGW_ARCH),mingw64)
WINDOWS_RUNTIME_DLLS := $(shell for pattern in libfreeglut.dll glew32.dll libportaudio.dll libwinpthread-1.dll libopus-0.dll libgcc_s_*.dll libstdc++-6.dll; do for f in "$(WINDOWS_MINGW_PREFIX)"/bin/$$pattern; do if [ -f "$$f" ]; then printf '%s ' "$$f"; fi; done; done)
else
WINDOWS_RUNTIME_DLLS := $(shell for pattern in libfreeglut.dll glew32.dll libwinpthread-1.dll libgcc_s_*.dll libstdc++-6.dll; do for f in "$(WINDOWS_MINGW_PREFIX)"/bin/$$pattern; do if [ -f "$$f" ]; then printf '%s ' "$$f"; fi; done; done) $(WINDOWS_OPUS_PORTAUDIO_DLLS)
endif
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
WINDOWS_RUNTIME_DLLS := $(shell for pattern in libwinpthread-1.dll libgcc_s_*.dll libstdc++-6.dll; do for f in "$(WINDOWS_MINGW_PREFIX)"/bin/$$pattern; do if [ -f "$$f" ]; then printf '%s ' "$$f"; fi; done; done) $(WINDOWS_OPUS_PORTAUDIO_DLLS)
endif
else
WINDOWS_RUNTIME_DLLS :=
endif
LDLIBS_DESKTOP_AUDIO := $(DESKTOP_PORTAUDIO_LINK)

# WinMM for timeBeginPeriod; MMCSS (avrt.dll) is loaded at runtime on Vista+ only — do not link -lavrt (no AVRT on XP/2K). See win32_timing_boost.c
WINDOWS_STREAMING_TIMING_LIBS := -lwinmm
LDLIBS_DESKTOP := -lopengl32 -lglew32 -lfreeglut $(LDLIBS_DESKTOP_AUDIO) $(WINDOWS_STREAMING_TIMING_LIBS) $(OPUS_LINK) -lpthread
NET_LIBS := -lws2_32 -liphlpapi
WINDOWS_GDI_LIBS := -lgdi32 -luser32
ifeq ($(WINDOWS_RETRO_BUNDLE),1)
LDLIBS_DESKTOP_RETRO := $(WINDOWS_STREAMING_TIMING_LIBS) $(DESKTOP_PORTAUDIO_LINK) $(OPUS_LINK) -lpthread $(NET_LIBS) $(WINDOWS_GDI_LIBS)
endif
else
# Linux / non-Windows: optional manual vendored codecs
ifeq ($(DASHCDG_OPUS_VENDOR),1)
ifneq ($(OPUS_VENDOR_PREFIX),)
ifneq ($(wildcard $(OPUS_VENDOR_PREFIX)/lib/libopus.a),)
OPUS_CPPFLAGS := -I$(OPUS_VENDOR_PREFIX)/include -DDASHCDG_OPUS_VENDOR_BUILD=1
OPUS_LINK := -L$(OPUS_VENDOR_PREFIX)/lib -lopus
endif
endif
endif
ifeq ($(DASHCDG_PORTAUDIO_VENDOR),1)
ifneq ($(PORTAUDIO_VENDOR_PREFIX),)
ifneq ($(wildcard $(PORTAUDIO_VENDOR_PREFIX)/lib/libportaudio.a),)
PORTAUDIO_CPPFLAGS := -I$(PORTAUDIO_VENDOR_PREFIX)/include -DDASHCDG_PORTAUDIO_VENDOR_BUILD=1
DESKTOP_PORTAUDIO_LINK := -L$(PORTAUDIO_VENDOR_PREFIX)/lib -lportaudio
endif
endif
endif
CFLAGS += $(OPUS_CPPFLAGS) $(PORTAUDIO_CPPFLAGS)
LDLIBS_DESKTOP := -lGL -lGLEW -lglut $(DESKTOP_PORTAUDIO_LINK) $(OPUS_LINK) -lpthread -lm
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
LDLIBS_DESKTOP_RX_GDI := $(LDLIBS_DESKTOP_AUDIO) $(WINDOWS_STREAMING_TIMING_LIBS) $(OPUS_LINK) -lpthread $(NET_LIBS) $(WINDOWS_GDI_LIBS)
LDLIBS_DESKTOP_TX_GDI := $(LDLIBS_DESKTOP_AUDIO) $(WINDOWS_STREAMING_TIMING_LIBS) $(OPUS_LINK) -lpthread $(NET_LIBS) $(WINDOWS_GDI_LIBS)
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
CODEC_AMR_NB_OBJS := $(patsubst audio_modules/amr/vendor/codec-amr/src/nb/%.c,$(OBJ_DIR)/amr_nb_%.o,$(wildcard audio_modules/amr/vendor/codec-amr/src/nb/*.c))
CODEC_AMR_WB_OBJS := $(patsubst audio_modules/amr/vendor/codec-amr/src/wb/%.c,$(OBJ_DIR)/amr_wb_%.o,$(wildcard audio_modules/amr/vendor/codec-amr/src/wb/*.c))
CODEC_AMR_DESKTOP_OBJS := $(OBJ_DIR)/desktop_amr_wb_codec.o $(OBJ_DIR)/desktop_amr_nb_codec.o

EVRCC_ROOT := audio_modules/evr/vendor/evrcc
EVRCC_CODE_EXCLUDE := $(EVRCC_ROOT)/code/main.c $(EVRCC_ROOT)/code/getopt.c
EVRCC_CODE_SRCS := $(filter-out $(EVRCC_CODE_EXCLUDE),$(wildcard $(EVRCC_ROOT)/code/*.c))
CODEC_EVRCC_CODE_OBJS := $(patsubst $(EVRCC_ROOT)/code/%.c,$(OBJ_DIR)/evrcc_code_%.o,$(EVRCC_CODE_SRCS))
CODEC_EVRCC_DSPMATH_OBJS := $(patsubst $(EVRCC_ROOT)/dspmath/%.c,$(OBJ_DIR)/evrcc_dspmath_%.o,$(wildcard $(EVRCC_ROOT)/dspmath/*.c))
CODEC_EVRCC_OBJS := $(CODEC_EVRCC_CODE_OBJS) $(CODEC_EVRCC_DSPMATH_OBJS) $(OBJ_DIR)/evrcc_dsp_fx_basic_op40.o $(OBJ_DIR)/evrcc_evrcc.o $(OBJ_DIR)/evrcc_evrcpacket.o
EVRCC_INCLUDES := -I$(EVRCC_ROOT) -I$(EVRCC_ROOT)/include -I$(EVRCC_ROOT)/code -I$(EVRCC_ROOT)/dspmath -I$(EVRCC_ROOT)/dsp_fx -Iaudio_modules/qcelp/vendor/celp13k/dsp_fx
EVRCC_CFLAGS := -DEVRC_BUILDING_DLL=1 -DDASHCDG_EVRC_USE_HOST_STDINT=1 $(EVRCC_INCLUDES) -Wno-unused-parameter -Wno-sign-compare -Wno-unknown-pragmas -fno-strict-aliasing

QCELP_ROOT := audio_modules/qcelp/vendor/celp13k
QCELP_CODE_EXCLUDE := code/celp13k.c code/io.c code/io_qcp.c code/fer_sim.c code/rate_dos.c code/ratedec_dos.c
QCELP_CODE_SRCS := $(filter-out $(addprefix $(QCELP_ROOT)/,$(QCELP_CODE_EXCLUDE)),$(wildcard $(QCELP_ROOT)/code/*.c))
CODEC_QCELP_CODE_OBJS := $(patsubst $(QCELP_ROOT)/code/%.c,$(OBJ_DIR)/qcelp_code_%.o,$(QCELP_CODE_SRCS))
CODEC_QCELP_TTY_OBJS := $(patsubst $(QCELP_ROOT)/tty/%.c,$(OBJ_DIR)/qcelp_tty_%.o,$(wildcard $(QCELP_ROOT)/tty/*.c))
QCELP_DSPFX_EXCLUDE := $(QCELP_ROOT)/dsp_fx/basic_op.c
CODEC_QCELP_DSP_OBJS := $(patsubst $(QCELP_ROOT)/dsp_fx/%.c,$(OBJ_DIR)/qcelp_dspfx_%.o,$(filter-out $(QCELP_DSPFX_EXCLUDE),$(wildcard $(QCELP_ROOT)/dsp_fx/*.c)))
CODEC_QCELP_OBJS := $(CODEC_QCELP_CODE_OBJS) $(CODEC_QCELP_TTY_OBJS) $(CODEC_QCELP_DSP_OBJS)
QCELP_CFLAGS := -I$(QCELP_ROOT)/code -I$(QCELP_ROOT)/tty -I$(QCELP_ROOT)/dsp_fx -Wno-unused-parameter -Wno-sign-compare -Wno-unknown-pragmas -fno-strict-aliasing

SBC_ROOT := audio_modules/bt_sbc/vendor/sbc
CODEC_SBC_OBJS := $(OBJ_DIR)/bt_sbc_sbc.o $(OBJ_DIR)/bt_sbc_sbc_primitives.o

DESKTOP_NB_CODEC_OBJS := $(OBJ_DIR)/desktop_nb_evrc_codec.o $(OBJ_DIR)/desktop_nb_qcelp_codec.o $(OBJ_DIR)/desktop_nb_sbc_codec.o

DESKTOP_LIB_OBJECTS := $(OBJ_DIR)/desktop_audio.o $(OBJ_DIR)/desktop_pcm_rate_convert.o $(OBJ_DIR)/desktop_cdg_source.o $(OBJ_DIR)/desktop_gl_renderer.o $(OBJ_DIR)/desktop_stream_runtime.o $(OBJ_DIR)/desktop_transport_udp.o $(OBJ_DIR)/desktop_win32_gdi_view.o $(OBJ_DIR)/desktop_win32_timing_boost.o $(CODEC_AMR_NB_OBJS) $(CODEC_AMR_WB_OBJS) $(CODEC_AMR_DESKTOP_OBJS) $(CODEC_EVRCC_OBJS) $(CODEC_QCELP_OBJS) $(CODEC_SBC_OBJS) $(DESKTOP_NB_CODEC_OBJS)
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

.PHONY: all debug dirs libs test desktop-apps bundle-runtime vendor-mingw32-p3-runtime package package-x64 package-x86 package-all-windows dist-windows dist-windows-sneakernet desktop-windows-x86-retro release clean

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
ifneq ($(wildcard vendor/windows-runtime/$(WINDOWS_ARCH_LABEL)/*.dll),)
	cp -f vendor/windows-runtime/$(WINDOWS_ARCH_LABEL)/*.dll $(BIN_DIR)/
endif
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

$(OBJ_DIR)/proto_protocol.o: proto/src/protocol.c proto/include/dashcdg/protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/proto_fec.o: proto/src/fec.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_file_io.o: platform/desktop/src/file_io.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_net_compat.o: platform/desktop/src/net_compat.c
	$(CC) $(CFLAGS) -c -o $@ $<

DESKTOP_AUDIO_CPPFLAGS ?=

$(OBJ_DIR)/desktop_audio.o: platform/desktop/src/desktop_audio.c
	$(CC) $(CFLAGS) $(DESKTOP_AUDIO_CPPFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_pcm_rate_convert.o: platform/desktop/src/pcm_rate_convert.c
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

$(OBJ_DIR)/desktop_win32_timing_boost.o: platform/desktop/src/win32_timing_boost.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_win32_gdi_view.o: platform/desktop/src/win32_gdi_view.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/amr_nb_%.o: audio_modules/amr/vendor/codec-amr/src/nb/%.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized -Wno-unknown-pragmas -fno-strict-aliasing -Iaudio_modules/amr/vendor/codec-amr/src/nb -Iaudio_modules/amr/vendor/codec-amr/src -c -o $@ $<

$(OBJ_DIR)/amr_wb_%.o: audio_modules/amr/vendor/codec-amr/src/wb/%.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized -Wno-unknown-pragmas -fno-strict-aliasing -Iaudio_modules/amr/vendor/codec-amr/src/wb -Iaudio_modules/amr/vendor/codec-amr/src -c -o $@ $<

$(OBJ_DIR)/desktop_amr_wb_codec.o: platform/desktop/src/amr_wb_codec.c
	$(CC) $(CFLAGS) -Iaudio_modules/amr/vendor/codec-amr/src/wb -Iaudio_modules/amr/vendor/codec-amr/src -c -o $@ $<

$(OBJ_DIR)/desktop_amr_nb_codec.o: platform/desktop/src/amr_nb_codec.c
	$(CC) $(CFLAGS) -Iaudio_modules/amr/vendor/codec-amr/src/nb -Iaudio_modules/amr/vendor/codec-amr/src -c -o $@ $<

$(OBJ_DIR)/evrcc_code_%.o: $(EVRCC_ROOT)/code/%.c
	$(CC) $(CFLAGS) $(EVRCC_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/evrcc_dspmath_%.o: $(EVRCC_ROOT)/dspmath/%.c
	$(CC) $(CFLAGS) $(EVRCC_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/evrcc_dsp_fx_basic_op40.o: $(EVRCC_ROOT)/dsp_fx/basic_op40.c
	$(CC) $(CFLAGS) $(EVRCC_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/evrcc_evrcc.o: $(EVRCC_ROOT)/evrcc.c
	$(CC) $(CFLAGS) $(EVRCC_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/evrcc_evrcpacket.o: $(EVRCC_ROOT)/evrcpacket.c
	$(CC) $(CFLAGS) $(EVRCC_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/qcelp_code_%.o: $(QCELP_ROOT)/code/%.c
	$(CC) $(CFLAGS) $(QCELP_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/qcelp_tty_%.o: $(QCELP_ROOT)/tty/%.c
	$(CC) $(CFLAGS) $(QCELP_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/qcelp_dspfx_%.o: $(QCELP_ROOT)/dsp_fx/%.c
	$(CC) $(CFLAGS) $(QCELP_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/bt_sbc_sbc.o: $(SBC_ROOT)/sbc/sbc.c
	$(CC) $(CFLAGS) -include stdint.h -I$(SBC_ROOT) -c -o $@ $<

$(OBJ_DIR)/bt_sbc_sbc_primitives.o: $(SBC_ROOT)/sbc/sbc_primitives.c
	$(CC) $(CFLAGS) -include stdint.h -I$(SBC_ROOT) -DDASHCDG_SBC_FORCE_GENERIC_PRIMITIVES=1 -c -o $@ $<

$(OBJ_DIR)/desktop_nb_evrc_codec.o: platform/desktop/src/nb_evrc_codec.c
	$(CC) $(CFLAGS) $(EVRCC_INCLUDES) -DEVRC_BUILDING_DLL=1 -DDASHCDG_EVRC_USE_HOST_STDINT=1 -c -o $@ $<

$(OBJ_DIR)/desktop_nb_qcelp_codec.o: platform/desktop/src/nb_qcelp_codec.c
	$(CC) $(CFLAGS) $(QCELP_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_nb_sbc_codec.o: platform/desktop/src/nb_sbc_codec.c
	$(CC) $(CFLAGS) -I$(SBC_ROOT) -c -o $@ $<

$(OBJ_DIR)/desktop_app_tx.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_tx_headless.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_TX_HEADLESS=1 -c -o $@ $<

ifneq ($(TX_GDI_BIN),)
$(OBJ_DIR)/desktop_app_tx_gdi.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_TX_GDI_PREVIEW=1 -c -o $@ $<
endif

$(OBJ_DIR)/desktop_app_tx_retro.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -DDASHCDG_DESKTOP_RETRO_WINDOWS=1 -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx_gdi.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -DDASHCDG_RX_UI_GDI_ONLY=1 -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx_retro_gdi.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -DDASHCDG_RX_UI_GDI_ONLY=1 -c -o $@ $<

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
$(RETRO_RX_BIN): $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_RETRO_GDI_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_RX_RETRO_GDI_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_RETRO)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(RETRO_TX_BIN): $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_RETRO_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_TX_RETRO_OBJECT) $(DESKTOP_OPUS_OBJECT) $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP_RETRO)
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

# Optional: clone vendored codec sources (network). Safe to re-run; skips existing dirs.
vendor-audio-sources:
	bash scripts/fetch_opus_portaudio_vendors.sh
	bash scripts/fetch_audio_codec_vendors.sh

# Build Pentium III–safe shared libopus-0.dll + libportaudio.dll into build/mingw32-p3-vendor/ (run vendor-audio-sources first if trees are missing).
vendor-mingw32-p3-runtime:
	bash scripts/build_mingw32_p3_opus_portaudio_shared.sh

# Full sneakernet matrix after fetching sources and building PIII-safe codecs.
dist-windows-sneakernet-with-sources: vendor-audio-sources vendor-mingw32-p3-runtime dist-windows-sneakernet

release: package

clean:
	rm -rf $(BUILD_DIR)
