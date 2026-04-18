#!/usr/bin/env bash
# Back-compat name: full PIII disassembly scan of sneakernet + vendored PIII + build/x86*/bin.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:-$ROOT/build/dist/dashcdg-windows-sneakernet}"
exec bash "$ROOT/scripts/verify_p3_pe_pentium3.sh" "$SD"
