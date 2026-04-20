#!/usr/bin/env bash
# Build static libsoxr into arch-specific prefixes (matches Makefile SOXR_VENDOR_PREFIX defaults).
#
#   bash scripts/build_soxr_vendor.sh mingw64   -> build/soxr-vendor-mingw64/install
#   bash scripts/build_soxr_vendor.sh mingw32   -> build/soxr-vendor-mingw32/install (PIII-safe: no SSE SIMD engines)
#
# CMake: resolved from PATH, standard MSYS2 locations, or installed automatically via pacman
# (mingw-w64-x86_64-cmake / mingw-w64-i686-cmake). Requires git and network on first run.
#
# Env: MSYS2_ROOT (override MSYS2 install dir), SOXR_INSTALL_PREFIX, SOXR_GIT_TAG, SOXR_GIT_REPO,
#      DASHCDG_SOXR_SKIP_PACMAN=1 — do not attempt pacman install (fail if cmake missing)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MINGW_ARCH="${1:-${MINGW_ARCH:-mingw64}}"
TAG="${SOXR_GIT_TAG:-0.1.3}"
REPO="${SOXR_GIT_REPO:-https://github.com/chirlu/soxr.git}"

dashcdg_find_msys_root() {
	local r c
	for r in "${MSYS2_ROOT:-}" "/c/msys64" "/c/msys2" "/d/msys64" "/d/msys2"; do
		[[ -z "${r:-}" ]] && continue
		r="${r%/}"
		if [[ -f "${r}/usr/bin/pacman.exe" || -f "${r}/usr/bin/pacman" ]]; then
			echo "${r}"
			return 0
		fi
	done
	c="$(command -v pacman 2>/dev/null || true)"
	if [[ -n "$c" ]]; then
		r="$(cd "$(dirname "$c")/../.." && pwd)"
		if [[ -f "${r}/usr/bin/pacman" || -f "${r}/usr/bin/pacman.exe" ]]; then
			echo "${r}"
			return 0
		fi
	fi
	echo ""
}

dashcdg_resolve_cmake() {
	local r cands c
	for c in "$(command -v cmake 2>/dev/null)" ""; do
		[[ -n "$c" && -x "$c" ]] && { echo "$c"; return 0; }
	done
	r="${MSYS_ROOT:-}"
	if [[ -n "$r" ]]; then
		case "${MINGW_ARCH}" in
		mingw64)
			cands=(
				"${r}/mingw64/bin/cmake.exe"
				"${r}/mingw64/bin/cmake"
				"${r}/usr/bin/cmake.exe"
				"${r}/usr/bin/cmake"
			)
			;;
		mingw32)
			cands=(
				"${r}/mingw32/bin/cmake.exe"
				"${r}/mingw32/bin/cmake"
				"${r}/usr/bin/cmake.exe"
				"${r}/usr/bin/cmake"
			)
			;;
		esac
		for c in "${cands[@]}"; do
			[[ -x "$c" || -f "$c" ]] && { echo "$c"; return 0; }
		done
	fi
	for c in "/c/Program Files/CMake/bin/cmake.exe" "/c/Program Files (x86)/CMake/bin/cmake.exe"; do
		[[ -x "$c" ]] && { echo "$c"; return 0; }
	done
	echo ""
}

dashcdg_ensure_cmake() {
	local cmake_exe pacman_bin pkg
	CMAKE="$(dashcdg_resolve_cmake)"
	if [[ -n "$CMAKE" ]]; then
		return 0
	fi
	if [[ "${DASHCDG_SOXR_SKIP_PACMAN:-0}" == "1" ]]; then
		echo "[soxr] cmake not found and DASHCDG_SOXR_SKIP_PACMAN=1" >&2
		return 1
	fi
	if [[ -z "${MSYS_ROOT:-}" ]]; then
		echo "[soxr] cmake not found and MSYS2 root unknown (set MSYS2_ROOT or install CMake)." >&2
		return 1
	fi
	pacman_bin="${MSYS_ROOT}/usr/bin/pacman.exe"
	[[ -f "$pacman_bin" ]] || pacman_bin="${MSYS_ROOT}/usr/bin/pacman"
	if [[ ! -f "$pacman_bin" ]]; then
		echo "[soxr] cmake missing and pacman not at ${MSYS_ROOT}/usr/bin/pacman" >&2
		return 1
	fi
	case "${MINGW_ARCH}" in
	mingw64) pkg="mingw-w64-x86_64-cmake" ;;
	mingw32) pkg="mingw-w64-i686-cmake" ;;
	*)
		echo "[soxr] internal: bad MINGW_ARCH" >&2
		return 1
		;;
	esac
	echo "[soxr] installing ${pkg} via pacman (one-time; requires MSYS2 mirrors)…" >&2
	if ! "$pacman_bin" -S --needed --noconfirm "$pkg"; then
		echo "[soxr] pacman install failed (another pacman running, or no network). Retry after: pacman -Syu" >&2
		return 1
	fi
	CMAKE="$(dashcdg_resolve_cmake)"
	if [[ -z "$CMAKE" ]]; then
		echo "[soxr] cmake still not found after installing ${pkg}" >&2
		return 1
	fi
	echo "[soxr] using cmake: ${CMAKE}" >&2
	return 0
}

MSYS_ROOT="$(dashcdg_find_msys_root)"
if [[ -z "$MSYS_ROOT" ]]; then
	MSYS_ROOT="${MSYS2_ROOT:-/c/msys64}"
fi

# soxr's CMakeLists uses EXISTS(${PROJECT_SOURCE_DIR}/...) in cmake_dependent_option — breaks when the path
# contains spaces (common under OneDrive). Clone and build under MSYS tmp; install .a back into the repo tree.
dashcdg_soxr_pick_workspace() {
	local h
	if [[ "${ROOT}" =~ [[:space:]] ]]; then
		if command -v sha256sum >/dev/null 2>&1; then
			h="$(printf '%s' "$ROOT" | sha256sum | awk '{print $1}' | cut -c1-16)"
		else
			h="ws"
		fi
		echo "${MSYS_ROOT}/tmp/dashcdg-soxr-${h}"
	fi
}

SOXR_WS="$(dashcdg_soxr_pick_workspace)"
if [[ -n "${SOXR_WS:-}" ]]; then
	mkdir -p "${SOXR_WS}"
	SRC_DIR="${SOXR_WS}/src"
	BUILD_DIR="${SOXR_WS}/cmake-build-${MINGW_ARCH}"
	echo "[soxr] repository path contains spaces — staging clone/build at ${SOXR_WS}" >&2
else
	SRC_DIR="${ROOT}/build/soxr-vendor/src"
	BUILD_DIR="${ROOT}/build/soxr-vendor/cmake-build-${MINGW_ARCH}"
fi

case "${MINGW_ARCH}" in
mingw64)
	export PATH="${MSYS_ROOT}/usr/bin:${MSYS_ROOT}/mingw64/bin:${PATH}"
	# Short names so CMake resolves the toolchain from PATH (absolute paths break some CMake 4 + MSYS2 combos).
	export CC="${CC:-x86_64-w64-mingw32-gcc}"
	export CXX="${CXX:-x86_64-w64-mingw32-g++}"
	INSTALL_DEFAULT="${ROOT}/build/soxr-vendor-mingw64/install"
	SIMD_FLAGS="-DWITH_CR32S=ON -DWITH_CR64S=ON"
	EXTRA_CFLAGS=""
	;;
mingw32)
	export PATH="${MSYS_ROOT}/usr/bin:${MSYS_ROOT}/mingw32/bin:${PATH}"
	export CC="${CC:-i686-w64-mingw32-gcc}"
	export CXX="${CXX:-i686-w64-mingw32-g++}"
	INSTALL_DEFAULT="${ROOT}/build/soxr-vendor-mingw32/install"
	SIMD_FLAGS="-DWITH_CR32S=OFF -DWITH_CR64S=OFF"
	EXTRA_CFLAGS="-march=pentium3 -mtune=pentium3 -mno-sse2 -mno-sse -mfpmath=387"
	;;
*)
	echo "[soxr] usage: $0 mingw64|mingw32" >&2
	exit 1
	;;
esac

CMAKE=""
if ! dashcdg_ensure_cmake; then
	exit 1
fi

INSTALL_PREFIX="${SOXR_INSTALL_PREFIX:-${INSTALL_DEFAULT}}"

if [[ ! -f "${SRC_DIR}/CMakeLists.txt" ]]; then
	rm -rf "${SRC_DIR}"
	mkdir -p "$(dirname "${SRC_DIR}")"
	echo "[soxr] clone ${REPO} (tag ${TAG})"
	git clone --depth 80 "${REPO}" "${SRC_DIR}"
	git -C "${SRC_DIR}" checkout "${TAG}"
fi

echo "[soxr] configuring ${MINGW_ARCH} -> ${INSTALL_PREFIX}"

# shellcheck disable=SC2086
"${CMAKE}" -S "${SRC_DIR}" -B "${BUILD_DIR}" \
	-G "Unix Makefiles" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="${EXTRA_CFLAGS}" \
	-DCMAKE_CXX_FLAGS="${EXTRA_CFLAGS}" \
	-DBUILD_SHARED_LIBS=OFF \
	-DBUILD_TESTS=OFF \
	-DBUILD_EXAMPLES=OFF \
	-DWITH_OPENMP=OFF \
	-DWITH_LSR_BINDINGS=OFF \
	-DWITH_PFFFT=OFF \
	${SIMD_FLAGS}

"${CMAKE}" --build "${BUILD_DIR}" --parallel
"${CMAKE}" --install "${BUILD_DIR}"

echo "[soxr] installed ${MINGW_ARCH} static lib to ${INSTALL_PREFIX}/lib/libsoxr.a"
