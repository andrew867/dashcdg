CC ?= gcc
AR ?= ar

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib

COMMON_CFLAGS := -Wall -Wextra -Wno-cpp -std=c99 -pedantic -D_FORTIFY_SOURCE=2
INCLUDES := -Icore/include -Iproto/include -Iplatform/desktop/include -Iinc
CFLAGS ?= $(COMMON_CFLAGS) $(INCLUDES)
UNAME_S := $(shell uname -s 2>/dev/null)

ifneq (,$(filter Windows_NT MINGW64_NT% MINGW32_NT% MSYS_NT%,$(OS) $(UNAME_S)))
LDLIBS_DESKTOP := -lopengl32 -lglew32 -lfreeglut -lportaudio -lpthread
NET_LIBS := -lws2_32
WINDOWS_RUNTIME_DLLS := /mingw64/bin/libfreeglut.dll /mingw64/bin/glew32.dll /mingw64/bin/libportaudio.dll /mingw64/bin/libwinpthread-1.dll
else
LDLIBS_DESKTOP := -lGL -lGLEW -lglut -lportaudio -lpthread
NET_LIBS :=
WINDOWS_RUNTIME_DLLS :=
endif

CORE_SOURCES := core/src/cdg.c core/src/media_clock.c
PROTO_SOURCES := proto/src/protocol.c
CORE_OBJECTS := $(OBJ_DIR)/core_cdg.o $(OBJ_DIR)/core_media_clock.o
PROTO_OBJECTS := $(OBJ_DIR)/proto_protocol.o
DESKTOP_COMMON_OBJECTS := $(OBJ_DIR)/desktop_file_io.o $(OBJ_DIR)/desktop_net_compat.o
DESKTOP_APP_OBJECTS := $(OBJ_DIR)/desktop_audio.o $(OBJ_DIR)/desktop_gl_renderer.o

CORE_LIB := $(LIB_DIR)/libdashcdg_core.a
PROTO_LIB := $(LIB_DIR)/libdashcdg_proto.a
DESKTOP_LIB := $(LIB_DIR)/libdashcdg_desktop.a

TEST_BIN := $(BIN_DIR)/test-core
PLAYER_BIN := $(BIN_DIR)/desktop-player
TX_BIN := $(BIN_DIR)/desktop-tx
RX_BIN := $(BIN_DIR)/desktop-rx

.PHONY: all debug dirs libs test desktop-apps clean

all: CFLAGS += -O2
all: dirs $(CORE_LIB) $(PROTO_LIB) $(TEST_BIN) $(TX_BIN)

debug: CFLAGS += -DDEBUG -g
debug: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(TEST_BIN) $(TX_BIN) $(PLAYER_BIN) $(RX_BIN)

desktop-apps: dirs $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(PLAYER_BIN) $(RX_BIN)

dirs:
	mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)

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

$(OBJ_DIR)/desktop_file_io.o: platform/desktop/src/file_io.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_net_compat.o: platform/desktop/src/net_compat.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_audio.o: platform/desktop/src/desktop_audio.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop_gl_renderer.o: platform/desktop/src/gl_renderer.c
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

$(PLAYER_BIN): $(OBJ_DIR)/app_desktop_player.o $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/app_desktop_player.o $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

$(TX_BIN): $(OBJ_DIR)/app_desktop_tx.o $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/app_desktop_tx.o $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_COMMON_OBJECTS) -lpthread $(NET_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f /mingw64/bin/libwinpthread-1.dll $(BIN_DIR)/
endif

$(RX_BIN): $(OBJ_DIR)/app_desktop_rx.o $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(DESKTOP_COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/app_desktop_rx.o $(CORE_LIB) $(PROTO_LIB) $(DESKTOP_LIB) $(DESKTOP_COMMON_OBJECTS) $(LDLIBS_DESKTOP) $(NET_LIBS)
ifneq ($(WINDOWS_RUNTIME_DLLS),)
	cp -f $(WINDOWS_RUNTIME_DLLS) $(BIN_DIR)/
endif

test: dirs $(CORE_LIB) $(PROTO_LIB) $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)
