#!/usr/bin/env bash
# Fetch libopus + PortAudio into audio_modules/*/vendor/
# (see docs/specs/vendored-opus-portaudio-windows.md).
#
# Opus default is the official release tarball (CMakeLists + legacy configure; vendor build uses CMake).
# To use a shallow git clone instead (needs autoreconf for autogen):  OPUS_VENDOR_FETCH=git
#
# Optional: OPUS_VENDOR_VERSION=1.5.2  (with tarball fetch)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OPUS_DIR="$ROOT/audio_modules/opus/vendor/opus"
PA_DIR="$ROOT/audio_modules/portaudio/vendor/portaudio"
OPUS_FETCH="${OPUS_VENDOR_FETCH:-tarball}"
OPUS_VER="${OPUS_VENDOR_VERSION:-1.5.2}"

opus_expected_sha256() {
  case "${1}" in
    1.5.2) echo "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1" ;;
    *) echo "" ;;
  esac
}

clone_ref() {
  local name="$1"
  local url="$2"
  local dir="$3"
  if [[ -d "$dir/.git" ]]; then
    echo "[fetch] skip (git exists): $name -> $dir"
    return 0
  fi
  mkdir -p "$(dirname "$dir")"
  echo "[fetch] clone: $name -> $dir"
  git clone --depth 1 "$url" "$dir"
}

fetch_opus_release_tarball() {
  if [[ -d "$OPUS_DIR/.git" ]]; then
    echo "[fetch] skip opus: git repo exists at $OPUS_DIR (remove or set OPUS_VENDOR_FETCH=git to refresh)."
    return 0
  fi
  if [[ -f "$OPUS_DIR/configure" || -f "$OPUS_DIR/CMakeLists.txt" ]]; then
    echo "[fetch] skip opus: upstream sources already present under $OPUS_DIR"
    return 0
  fi
  local tb="opus-${OPUS_VER}.tar.gz"
  local url="https://github.com/xiph/opus/releases/download/v${OPUS_VER}/${tb}"
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/dashcdg-opus-fetch.XXXXXX")"
  echo "[fetch] opus release tarball v${OPUS_VER}"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$tmp/$tb" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tmp/$tb" "$url"
  else
    echo "[fetch] need curl or wget to download $url" >&2
    rm -rf "$tmp"
    exit 1
  fi
  local want
  want="$(opus_expected_sha256 "$OPUS_VER")"
  if [[ -n "$want" ]]; then
    local got
    if command -v sha256sum >/dev/null 2>&1; then
      got="$(sha256sum "$tmp/$tb" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
      got="$(shasum -a 256 "$tmp/$tb" | awk '{print $1}')"
    else
      got=""
    fi
    if [[ -n "$got" && "$got" != "$want" ]]; then
      echo "[fetch] SHA256 mismatch for $tb (got $got want $want)" >&2
      rm -rf "$tmp"
      exit 1
    fi
  fi
  rm -rf "$OPUS_DIR"
  mkdir -p "$OPUS_DIR"
  tar -xzf "$tmp/$tb" -C "$OPUS_DIR" --strip-components=1
  rm -rf "$tmp"
  if [[ ! -f "$OPUS_DIR/configure" && ! -f "$OPUS_DIR/CMakeLists.txt" ]]; then
    echo "[fetch] opus extract failed: need CMakeLists.txt or configure under $OPUS_DIR" >&2
    exit 1
  fi
  echo "[fetch] opus tarball ready under $OPUS_DIR"
}

case "${OPUS_FETCH}" in
  git)
    clone_ref "opus (xiph)" "https://github.com/xiph/opus.git" "$OPUS_DIR"
    ;;
  tarball)
    fetch_opus_release_tarball
    ;;
  *)
    echo "[fetch] OPUS_VENDOR_FETCH must be tarball or git, got: ${OPUS_FETCH}" >&2
    exit 1
    ;;
esac

clone_ref "portaudio" "https://github.com/PortAudio/portaudio.git" "$PA_DIR"

echo "[fetch] Done. Next: bash scripts/build_mingw32_p3_opus_portaudio_shared.sh"
