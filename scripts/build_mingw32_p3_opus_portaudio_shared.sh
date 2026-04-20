#!/usr/bin/env bash
# Build Pentium III / pre-SSE2–safe shared libopus-0.dll and libportaudio.dll for MinGW i686.
# Installs under build/mingw32-p3-vendor/{opus,portaudio} by default (matches Makefile defaults).
#
# Requires vendor trees from: bash scripts/fetch_opus_portaudio_vendors.sh
# Usage (MSYS2, MINGW32 shell):  ./scripts/build_mingw32_p3_opus_portaudio_shared.sh
#
# Prerequisites: mingw-w64-i686-{gcc,tools} and mingw-w64-i686-cmake (Ninja is pulled in).
# Optional: OPUS_VENDOR_PREFIX PORTAUDIO_VENDOR_PREFIX JOBS MSYS2_ROOT
#
# Incremental (default): if installed DLLs + stamp under each prefix match this script's
# configuration and vendor CMakeLists.txt is not newer than the DLL, skip that codec rebuild.
# Force full rebuild: DASHCDG_P3_VENDOR_REBUILD=1
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Autoconf rejects some /tmp paths as --prefix on MSYS2 (breaks as absolute: "prefix: 0"); use MSYS2's own tmp.
dashcdg_p3_safe_tmp() {
  local p="${MSYS2_ROOT:-}"
  for p in "${p}" /c/msys64 /c/msys2; do
    [[ -z "$p" ]] && continue
    if [[ -d "$p/tmp" ]]; then
      echo "$p/tmp"
      return 0
    fi
  done
  echo "/c/msys64/tmp"
}

# Libtool + MinGW need a single coherent MSYS2 mingw32 toolchain; PATH entries like
# ProgramData/.../mingw32-make or "C:/Program Files/..." shims break libtool with "C:/Program: No such file".
dashcdg_try_pin_mingw32_at_base() {
  local base="$1"
  local gcc_exe cxx_exe
  [[ -z "$base" ]] && return 1
  gcc_exe="$base/mingw32/bin/i686-w64-mingw32-gcc.exe"
  if [[ ! -f "$gcc_exe" ]]; then
    gcc_exe="$base/mingw32/bin/i686-w64-mingw32-gcc"
  fi
  if [[ ! -f "$gcc_exe" ]]; then
    return 1
  fi
  export DASHCDG_MSYS2_ROOT="$base"
  export MSYS2_ROOT="$base"
  export PATH="$base/mingw32/bin:$base/usr/bin:${PATH:-}"
  export CC="$gcc_exe"
  cxx_exe="$base/mingw32/bin/i686-w64-mingw32-g++.exe"
  [[ -f "$cxx_exe" ]] || cxx_exe="$base/mingw32/bin/i686-w64-mingw32-g++"
  export CXX="$cxx_exe"
  if [[ -x "$base/mingw32/bin/mingw32-make" || -f "$base/mingw32/bin/mingw32-make.exe" ]]; then
    export MAKE="$base/mingw32/bin/mingw32-make"
  else
    export MAKE="/usr/bin/make"
  fi
  # Avoid Git / Windows shims that split on spaces inside libtool wrappers
  export SHELL="/usr/bin/bash"
  export CONFIG_SHELL="/usr/bin/bash"
  return 0
}

dashcdg_pin_msys2_mingw32_toolchain() {
  local root base gcc64 bin64 derived
  root="${MSYS2_ROOT:-}"
  for base in "${root}" /c/msys64 /c/msys2; do
    [[ -z "$base" ]] && continue
    dashcdg_try_pin_mingw32_at_base "$base" && return 0
  done
  # GitHub Actions / portable MSYS2: prefix is not always /c/msys64 — derive from mingw64 gcc on PATH.
  gcc64="$(command -v x86_64-w64-mingw32-gcc 2>/dev/null || true)"
  if [[ -n "$gcc64" ]]; then
    bin64="$(cd "$(dirname "$gcc64")" && pwd)"
    derived="$(cd "$bin64/../.." && pwd)"
    dashcdg_try_pin_mingw32_at_base "$derived" && return 0
  fi
  return 1
}

if ! dashcdg_pin_msys2_mingw32_toolchain; then
  echo "[p3-vendor] Need MSYS2 mingw32 i686 toolchain (e.g. C:/msys64/mingw32/bin). Install with:" >&2
  echo "  pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-make make (make falls back to /usr/bin/make)" >&2
  exit 1
fi

# MSYS2: autoreconf/automake live in usr/bin. Git-for-Windows MINGW64 uses Git's mingw64\bin for gcc
# (two levels up is .../Git, not MSYS2). Probe MSYS2 roots (any drive / Scoop) and use -e not only -x
# (Git Bash sometimes mis-reports execute bit on MSYS perl/sh stubs).
# Optional override: MSYS2_ROOT=C:/msys64 (or /c/msys64).
dashcdg_try_autotools_usr_bin() {
  local bindir="${1%/}"
  local ar
  [[ -z "$bindir" ]] && return 1
  ar="$bindir/autoreconf"
  [[ -f "$ar" || -x "$ar" || -e "$ar" ]] || return 1
  export PATH="$bindir:$PATH"
  hash -r
  command -v autoreconf >/dev/null 2>&1
}

dashcdg_try_autotools_from_tool_on_path() {
  local tool="$1"
  local tool_path bin_dir msys_root
  tool_path="$(command -v "$tool" 2>/dev/null || true)"
  [[ -z "$tool_path" ]] && return 1
  bin_dir="$(cd "$(dirname "$tool_path")" && pwd)"
  msys_root="$(cd "$bin_dir/../.." && pwd)"
  dashcdg_try_autotools_usr_bin "$msys_root/usr/bin"
}

dashcdg_prepend_autotools_path() {
  local gcc_path bin_dir msys_root letter root d la tool

  if command -v autoreconf >/dev/null 2>&1; then
    return 0
  fi

  # Real MSYS2 MinGW shells set MSYSTEM; autotools live in /usr/bin but are often omitted from PATH.
  if [[ -n "${MSYSTEM:-}" ]] && dashcdg_try_autotools_usr_bin "/usr/bin"; then
    return 0
  fi

  for tool in pacman mingw32-make mingw64-make i686-w64-mingw32-gcc x86_64-w64-mingw32-gcc; do
    dashcdg_try_autotools_from_tool_on_path "$tool" && return 0
  done

  # Explicit / well-known paths first (Chocolatey uses /c/tools/msys64, etc.)
  for d in \
    "${MSYS2_ROOT:+$MSYS2_ROOT/usr/bin}" \
    "/usr/bin" \
    "/c/msys64/usr/bin" \
    "/c/msys2/usr/bin" \
    "/c/tools/msys64/usr/bin" \
    "/c/tools/msys2/usr/bin" \
    "/d/msys64/usr/bin" \
    "/d/msys2/usr/bin" \
    "/c/Program Files/msys64/usr/bin" \
    "/c/Program Files/msys2/usr/bin"; do
    [[ -z "$d" ]] && continue
    dashcdg_try_autotools_usr_bin "$d" && return 0
  done

  la=""
  if [[ -n "${LOCALAPPDATA:-}" ]]; then
    if [[ "${LOCALAPPDATA}" =~ ^[A-Za-z]:[\\/] ]] || [[ "${LOCALAPPDATA}" == *\\* ]]; then
      la="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || true)"
    else
      la="${LOCALAPPDATA}"
    fi
  fi
  if [[ -n "$la" ]]; then
    for d in "$la/msys64/usr/bin" "$la/msys2/usr/bin"; do
      dashcdg_try_autotools_usr_bin "$d" && return 0
    done
  fi

  # Any drive letter (MSYS2 on E:, portable installs, etc.)
  for letter in {a..z}; do
    for root in msys64 msys2; do
      d="/${letter}/${root}/usr/bin"
      dashcdg_try_autotools_usr_bin "$d" && return 0
    done
  done

  # Scoop shims don't include autoreconf; real prefix is under apps/msys2/<ver>/usr/bin
  if [[ -d "${HOME}/scoop/apps/msys2" ]]; then
    shopt -s nullglob
    for d in "${HOME}/scoop/apps/msys2/current/usr/bin" "${HOME}/scoop/apps/msys2"/*/usr/bin; do
      dashcdg_try_autotools_usr_bin "$d" && return 0
    done
    shopt -u nullglob
  fi

  gcc_path="$(command -v gcc 2>/dev/null || true)"
  if [[ -z "$gcc_path" ]]; then
    return 1
  fi
  bin_dir="$(cd "$(dirname "$gcc_path")" && pwd)"
  # .../<msys>/mingw64|ucrt64|clang64|mingw32/bin -> .../<msys>
  msys_root="$(cd "$bin_dir/../.." && pwd)"
  dashcdg_try_autotools_usr_bin "$msys_root/usr/bin" && return 0

  return 1
}

dashcdg_prepend_autotools_path || true

export CC CXX MAKE
# Pentium III class: no SSE2 in third-party code (match dashcdg -march=pentium3 objects).
# -mno-sse on top of -march=p3 -mno-sse2: helps keep scalar FP in x87 for third-party C (PortAudio, etc.).
# The Opus float path (silk/.../float/*) still tripped the PIII disasm gate (cvttsd2si) on GCC 15, so we also
# build the codec in fixed point (PIII/embedded-friendly; public DLL API is unchanged for libopus use).
# Pentium MMX and older: use -march=pentium-mmx in a custom build if you must; Opus is not tuned for that here.
# Keep this one shell word per flag: some MSYS sh/configure paths misparsed combined options as --enable-0.
P3_CFLAGS="-O2 -march=pentium3 -mtune=pentium3 -mno-sse -mno-sse2 -mfpmath=387 -fno-tree-vectorize -fno-tree-slp-vectorize -U_FORTIFY_SOURCE"
# CMake appends CMAKE_C_FLAGS_RELEASE to CMAKE_C_FLAGS for -DCMAKE_BUILD_TYPE=Release. The MinGW
# default for that is often -O3, which can re-open vectorization (SSE2) after -fno-tree-vectorize
# in CMAKE_C_FLAGS. Put the full PIII set in CFLAGS and turn off the extra Release layer.
P3_CMAKE_C_FLAGS="${P3_CFLAGS} -DNDEBUG"
# Empty Release flags so CMake does not add the toolchain default -O3 (see verify_p3 on libopus).
P3_CMAKE_FLAGS_RELEASE_INIT=""

OPUS_SRC="$ROOT/audio_modules/opus/vendor/opus"
PA_SRC="$ROOT/audio_modules/portaudio/vendor/portaudio"
OPUS_PREFIX="${OPUS_VENDOR_PREFIX:-$ROOT/build/mingw32-p3-vendor/opus}"
PA_PREFIX="${PORTAUDIO_VENDOR_PREFIX:-$ROOT/build/mingw32-p3-vendor/portaudio}"

# Autoconf/Make install cannot reliably handle spaces in --prefix (OneDrive paths).
OPUS_INST="$OPUS_PREFIX"
PA_INST="$PA_PREFIX"
if [[ "$OPUS_PREFIX" == *' '* ]]; then
  OPUS_INST="$(dashcdg_p3_safe_tmp)/dashcdg-p3-i-opus-$$"
  rm -rf "$OPUS_INST"
  mkdir -p "$OPUS_INST"
  echo "[p3-vendor] OPUS prefix has spaces — installing to $OPUS_INST then copying to repo." >&2
fi
if [[ "$PA_PREFIX" == *' '* ]]; then
  PA_INST="$(dashcdg_p3_safe_tmp)/dashcdg-p3-i-pa-$$"
  rm -rf "$PA_INST"
  mkdir -p "$PA_INST"
  echo "[p3-vendor] PortAudio prefix has spaces — installing to $PA_INST then copying to repo." >&2
fi

STAGE_DIR=""
OPUS_WORK="$OPUS_SRC"
PA_WORK="$PA_SRC"
# libtool does not tolerate spaces in pwd; many users keep the repo under "OneDrive - ...".
if [[ "$ROOT" == *' '* ]]; then
  STAGE_ROOT="${DASHCDG_P3_STAGING:-$(dashcdg_p3_safe_tmp)}"
  mkdir -p "$STAGE_ROOT" 2>/dev/null || STAGE_ROOT="/c/msys64/tmp"
  STAGE_DIR="$(mktemp -d "${STAGE_ROOT%/}/dashcdg-p3-XXXXXX")"
  echo "[p3-vendor] repository path contains spaces — Opus/PortAudio builds use staging dir:" >&2
  echo "  $STAGE_DIR" >&2
  rm -rf "$STAGE_DIR/opus" "$STAGE_DIR/portaudio"
  mkdir -p "$STAGE_DIR/opus" "$STAGE_DIR/portaudio"
  cp -a "$OPUS_SRC/." "$STAGE_DIR/opus/"
  cp -a "$PA_SRC/." "$STAGE_DIR/portaudio/"
  OPUS_WORK="$STAGE_DIR/opus"
  PA_WORK="$STAGE_DIR/portaudio"
fi

if [[ ! -f "$OPUS_SRC/CMakeLists.txt" ]]; then
  echo "[p3-vendor] Missing Opus sources at $OPUS_SRC (need CMakeLists.txt)" >&2
  echo "  Run:  bash scripts/fetch_opus_portaudio_vendors.sh" >&2
  exit 1
fi
if [[ ! -f "$PA_SRC/CMakeLists.txt" ]]; then
  echo "[p3-vendor] Missing PortAudio sources at $PA_SRC" >&2
  echo "  Run:  bash scripts/fetch_opus_portaudio_vendors.sh" >&2
  exit 1
fi

# Bump embedded config version if CMake options below change (invalidates skip stamps).
P3_OPUS_CFG_SIG="opus-p3-v2|${P3_CMAKE_C_FLAGS}|SHARED|FIXED_POINT|NO_INTRINSICS|NO_FLOAT_API"
P3_PA_CFG_SIG="portaudio-p3-v2|${P3_CMAKE_C_FLAGS}|SHARED|WMME|DSOUND|NO_WASAPI"
if command -v sha256sum >/dev/null 2>&1; then
  P3_OPUS_BUILD_ID="$(printf '%s' "$P3_OPUS_CFG_SIG" | sha256sum | awk '{print $1}')"
  P3_PA_BUILD_ID="$(printf '%s' "$P3_PA_CFG_SIG" | sha256sum | awk '{print $1}')"
else
  P3_OPUS_BUILD_ID="$P3_OPUS_CFG_SIG"
  P3_PA_BUILD_ID="$P3_PA_CFG_SIG"
fi

mkdir -p "$OPUS_PREFIX"
if ! command -v cmake >/dev/null 2>&1; then
  echo "[p3-vendor] cmake not found on PATH (need MinGW i686 CMake next to CC)." >&2
  echo "  MSYS2 MINGW32:  pacman -S --needed mingw-w64-i686-cmake" >&2
  exit 1
fi

OPUS_DLL=""
[[ -f "${OPUS_PREFIX}/bin/libopus-0.dll" ]] && OPUS_DLL="${OPUS_PREFIX}/bin/libopus-0.dll"
[[ -z "$OPUS_DLL" && -f "${OPUS_PREFIX}/bin/libopus.dll" ]] && OPUS_DLL="${OPUS_PREFIX}/bin/libopus.dll"
OPUS_STAMP="${OPUS_PREFIX}/.dashcdg_p3_opus_build_id"
OPUS_SKIP=0
if [[ "${DASHCDG_P3_VENDOR_REBUILD:-0}" != "1" ]] && [[ -n "$OPUS_DLL" ]] && [[ -f "$OPUS_STAMP" ]] &&
    [[ "$(cat "$OPUS_STAMP" 2>/dev/null || true)" == "$P3_OPUS_BUILD_ID" ]] &&
    [[ ! "$OPUS_SRC/CMakeLists.txt" -nt "$OPUS_DLL" ]]; then
  echo "[p3-vendor] skip: Opus shared lib already up to date ($OPUS_DLL). Set DASHCDG_P3_VENDOR_REBUILD=1 to rebuild."
  OPUS_SKIP=1
fi

if [[ "$OPUS_SKIP" -eq 0 ]]; then
  echo "[p3-vendor] Building shared libopus (CMake) -> $OPUS_PREFIX"
  OPUS_BUILD="$OPUS_WORK/build-dashcdg-mingw32-p3-shared"
  rm -rf "$OPUS_BUILD"
  cmake -S "$OPUS_WORK" -B "$OPUS_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$OPUS_INST" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_C_FLAGS:STRING="${P3_CMAKE_C_FLAGS}" \
  -DCMAKE_C_FLAGS_RELEASE:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_C_FLAGS_MINSIZEREL:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DOPUS_BUILD_SHARED_LIBRARY=ON \
  -DOPUS_DISABLE_INTRINSICS=ON \
  -DOPUS_FIXED_POINT=ON \
  -DOPUS_ENABLE_FLOAT_API=OFF \
  -DOPUS_BUILD_TESTING=OFF \
  -DOPUS_BUILD_PROGRAMS=OFF \
  -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF \
  -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF

  cmake --build "$OPUS_BUILD" --parallel "${JOBS:-8}"
  cmake --install "$OPUS_BUILD"

  if [[ "$OPUS_INST" != "$OPUS_PREFIX" ]]; then
    mkdir -p "$OPUS_PREFIX"
    cp -a "$OPUS_INST/." "$OPUS_PREFIX/"
    rm -rf "$OPUS_INST"
  fi
  printf '%s' "$P3_OPUS_BUILD_ID" > "$OPUS_STAMP"
fi

# GNU make looks for libopus-0.dll.a or libopus.dll.a; duplicate so either wildcard matches.
if [[ -d "$OPUS_PREFIX/lib" ]]; then
  if [[ -f "$OPUS_PREFIX/lib/libopus-0.dll.a" && ! -f "$OPUS_PREFIX/lib/libopus.dll.a" ]]; then
    cp -a "$OPUS_PREFIX/lib/libopus-0.dll.a" "$OPUS_PREFIX/lib/libopus.dll.a"
  elif [[ -f "$OPUS_PREFIX/lib/libopus.dll.a" && ! -f "$OPUS_PREFIX/lib/libopus-0.dll.a" ]]; then
    cp -a "$OPUS_PREFIX/lib/libopus.dll.a" "$OPUS_PREFIX/lib/libopus-0.dll.a"
  fi
fi

# MinGW `-lopus` + Windows loader expect `libopus-0.dll` beside the EXE; Opus CMake may install only `libopus.dll`.
if [[ -f "$OPUS_PREFIX/bin/libopus.dll" && ! -f "$OPUS_PREFIX/bin/libopus-0.dll" ]]; then
  cp -f "$OPUS_PREFIX/bin/libopus.dll" "$OPUS_PREFIX/bin/libopus-0.dll"
  echo "[p3-vendor] mirrored libopus.dll → libopus-0.dll (runtime name for dashcdg bundles)" >&2
fi

cd "$ROOT"

PA_DLL="${PA_PREFIX}/bin/libportaudio.dll"
PA_STAMP="${PA_PREFIX}/.dashcdg_p3_portaudio_build_id"
PA_SKIP=0
if [[ "${DASHCDG_P3_VENDOR_REBUILD:-0}" != "1" ]] && [[ -f "$PA_DLL" ]] && [[ -f "$PA_STAMP" ]] &&
    [[ "$(cat "$PA_STAMP" 2>/dev/null || true)" == "$P3_PA_BUILD_ID" ]] &&
    [[ ! "$PA_SRC/CMakeLists.txt" -nt "$PA_DLL" ]]; then
  echo "[p3-vendor] skip: PortAudio shared lib already up to date ($PA_DLL). Set DASHCDG_P3_VENDOR_REBUILD=1 to rebuild."
  PA_SKIP=1
fi

if [[ "$PA_SKIP" -eq 0 ]]; then
  echo "[p3-vendor] Building shared PortAudio -> $PA_PREFIX"
  PA_BUILD="$PA_WORK/build-dashcdg-mingw32-p3-shared"
  rm -rf "$PA_BUILD"
  # WASAPI off: friendlier to Windows 2000 / older hosts; WMME + DSOUND remain.
  cmake -S "$PA_WORK" -B "$PA_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PA_INST" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS:STRING="${P3_CMAKE_C_FLAGS}" \
  -DCMAKE_CXX_FLAGS:STRING="${P3_CMAKE_C_FLAGS}" \
  -DCMAKE_C_FLAGS_RELEASE:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_CXX_FLAGS_RELEASE:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_C_FLAGS_MINSIZEREL:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_CXX_FLAGS_MINSIZEREL:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING="${P3_CMAKE_FLAGS_RELEASE_INIT}" \
  -DBUILD_SHARED_LIBS=ON \
  -DPA_BUILD_TESTS=OFF \
  -DPA_BUILD_EXAMPLES=OFF \
  -DPA_USE_WMME=ON \
  -DPA_USE_DSOUND=ON \
  -DPA_USE_WASAPI=OFF \
  -DPA_USE_ASIO=OFF

  cmake --build "$PA_BUILD" --parallel "${JOBS:-8}"
  cmake --install "$PA_BUILD"

  if [[ "$PA_INST" != "$PA_PREFIX" ]]; then
    mkdir -p "$PA_PREFIX"
    cp -a "$PA_INST/." "$PA_PREFIX/"
    rm -rf "$PA_INST"
  fi
  printf '%s' "$P3_PA_BUILD_ID" > "$PA_STAMP"
fi

if [[ -n "$STAGE_DIR" ]]; then
  rm -rf "$STAGE_DIR"
fi

if [[ ! -f "$OPUS_PREFIX/bin/libopus-0.dll" && ! -f "$OPUS_PREFIX/bin/libopus.dll" ]]; then
  echo "[p3-vendor] warning: expected libopus DLL under $OPUS_PREFIX/bin — check CMake install output" >&2
fi
if [[ ! -f "$PA_PREFIX/bin/libportaudio.dll" ]]; then
  echo "[p3-vendor] error: missing $PA_PREFIX/bin/libportaudio.dll" >&2
  exit 1
fi

echo "[p3-vendor] OK: Opus + PortAudio shared libraries installed."
echo "  Opus:     $OPUS_PREFIX/bin/"
echo "  PortAudio: $PA_PREFIX/bin/"
if [[ "${SKIP_P3_CODEC_VERIFY:-}" != "1" ]]; then
  OPUS_VERIFY="${OPUS_PREFIX}/bin/libopus-0.dll"
  [[ -f "$OPUS_VERIFY" ]] || OPUS_VERIFY="${OPUS_PREFIX}/bin/libopus.dll"
  bash "${ROOT}/scripts/verify_mingw32_p3_codec_dlls.sh" "$OPUS_VERIFY" "${PA_PREFIX}/bin/libportaudio.dll"
fi
