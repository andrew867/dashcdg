CC ?= gcc
AR ?= ar

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib
RELEASE_DIR := $(BUILD_DIR)/release
WINDOWS_PACKAGE_ZIP := $(RELEASE_DIR)/dashcdg-windows-portable.zip

COMMON_CFLAGS := -Wall -Wextra -Wno-cpp -std=c99 -pedantic -D_FORTIFY_SOURCE=2
INCLUDES := -Icore/include -Iproto/include -Iplatform/desktop/include -Iinc
EXTRA_LDFLAGS :=
CFLAGS ?= $(COMMON_CFLAGS) $(INCLUDES)
UNAME_S := $(shell uname -s 2>/dev/null)

ifneq (,$(filter Windows_NT MINGW64_NT% MINGW32_NT% MSYS_NT%,$(OS) $(UNAME_S)))
WINDOWS_MINGW_PREFIX := $(shell if [ -d /c/msys64/mingw64 ]; then echo /c/msys64/mingw64; elif [ -d /c/ProgramData/mingw64/mingw64 ]; then echo /c/ProgramData/mingw64/mingw64; elif [ -d /mingw64 ]; then echo /mingw64; fi)
WINDOWS_MINGW_PREFIX_WIN := $(shell if [ -n "$(WINDOWS_MINGW_PREFIX)" ]; then cygpath -m "$(WINDOWS_MINGW_PREFIX)"; fi)
ifneq ($(WINDOWS_MINGW_PREFIX),)
INCLUDES += -I$(WINDOWS_MINGW_PREFIX_WIN)/include
EXTRA_LDFLAGS += -L$(WINDOWS_MINGW_PREFIX_WIN)/lib
WINDOWS_RUNTIME_DLLS := $(shell for f in libfreeglut.dll glew32.dll libportaudio.dll libwinpthread-1.dll libopus-0.dll; do if [ -f "$(WINDOWS_MINGW_PREFIX)/bin/$$f" ]; then printf '%s ' "$(WINDOWS_MINGW_PREFIX)/bin/$$f"; fi; done)
else
WINDOWS_RUNTIME_DLLS :=
endif
LDLIBS_DESKTOP := -lopengl32 -lglew32 -lfreeglut -lportaudio -lopus -lpthread
NET_LIBS := -lws2_32 -liphlpapi
else
LDLIBS_DESKTOP := -lGL -lGLEW -lglut -lportaudio -lopus -lpthread
NET_LIBS :=
WINDOWS_RUNTIME_DLLS :=
endif

CORE_SOURCES := core/src/cdg.c core/src/media_clock.c
PROTO_SOURCES := proto/src/protocol.c proto/src/fec.c
CORE_OBJECTS := $(OBJ_DIR)/core_cdg.o $(OBJ_DIR)/core_media_clock.o
PROTO_OBJECTS := $(OBJ_DIR)/proto_protocol.o $(OBJ_DIR)/proto_fec.o
DESKTOP_COMMON_OBJECTS := $(OBJ_DIR)/desktop_file_io.o $(OBJ_DIR)/desktop_net_compat.o
DESKTOP_APP_OBJECTS := $(OBJ_DIR)/desktop_audio.o $(OBJ_DIR)/desktop_opus_codec.o $(OBJ_DIR)/desktop_cdg_source.o $(OBJ_DIR)/desktop_gl_renderer.o $(OBJ_DIR)/desktop_stream_runtime.o $(OBJ_DIR)/desktop_app_tx.o $(OBJ_DIR)/desktop_app_rx.o

CORE_LIB := $(LIB_DIR)/libdashcdg_core.a
PROTO_LIB := $(LIB_DIR)/libdashcdg_proto.a
DESKTOP_LIB := $(LIB_DIR)/libdashcdg_desktop.a

TEST_BIN := $(BIN_DIR)/test-core
PLAYER_BIN := $(BIN_DIR)/desktop-player
TX_BIN := $(BIN_DIR)/desktop-tx
RX_BIN := $(BIN_DIR)/desktop-rx

.PHONY: all debug dirs libs test desktop-apps bundle-runtime package release clean

all: CFLAGS += -O2
all: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(TEST_BIN) $(TX_BIN)

debug: CFLAGS += -DDEBUG -g
debug: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(TEST_BIN) $(TX_BIN) $(PLAYER_BIN) $(RX_BIN)

desktop-apps: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(PLAYER_BIN) $(RX_BIN)

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

$(DESKTOP_LIB): $(DESKTOP_APP_OBJECTS)
	$(AR) rcs $@ $^

$(OBJ_DIR)/core_cdg.o: core/src/cdg.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/core_media_clock.o: core/src/media_clock.c
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

$(OBJ_DIR)/desktop_cdg_source.o: platform/desktop/src/cdg_source.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_gl_renderer.o: platform/desktop/src/gl_renderer.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_stream_runtime.o: platform/desktop/src/stream_runtime.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_tx.o: platform/desktop/src/app_tx.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_app_rx.o: platform/desktop/src/app_rx.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/test_core.o: tests/test_core.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/app_desktop_player.o: apps/desktop-player/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/app_desktop_tx.o: apps/desktop-tx/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/app_desktop_rx.o: apps/desktop-rx/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(OBJ_DIR)/test_core.o $(CORE_LIB) $(PROTO_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/test_core.o $(CORE_LIB) $(PROTO_LIB)

$(PLAYER_BIN): $(OBJ_DIR)/app_desktop_player.o $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_player.o $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(TX_BIN): $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(RX_BIN): $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJ_DIR)/app_desktop_rx.o $(DESKTOP_LIB) $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

test: dirs $(CORE_LIB) $(PROTO_LIB) $(TEST_BIN)
	$(TEST_BIN)

package: debug
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	rm -f "$(WINDOWS_PACKAGE_ZIP)"
	powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path 'build/bin/*' -DestinationPath 'build/release/dashcdg-windows-portable.zip' -Force"
else
	@echo "package target currently supports Windows/MSYS2 desktop bundles only" && false
endif

release: package

clean:
	rm -rf $(BUILD_DIR)
