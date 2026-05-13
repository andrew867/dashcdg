#!/usr/bin/env python3
"""
Automated ESP32 badge debug cycle:
1) Initialize ESP-IDF environment
2) Build + flash
3) Capture UART logs for a fixed duration
4) Repeat for N iterations
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_IDF_ID = "esp-idf-20ee62e792ea89630ac6a777ab3ebc57"
DEFAULT_PORT = "COM6"
DEFAULT_BAUD = 115200
DEFAULT_LOG_SECONDS = 45
DEFAULT_ITERATIONS = 1
DEFAULT_WAIT_DHCP_SECONDS = 30
DEFAULT_BUILD_DIR_WIN = str(Path(os.environ.get("TEMP", r"C:\Windows\Temp")) / "dashcdg_badge_build")


def windows_shell_env() -> dict[str, str]:
    env = dict(os.environ)
    for k in list(env):
        ku = k.upper()
        if "MSYS" in ku or "MINGW" in ku or "CYGWIN" in ku:
            env.pop(k, None)
    for k in ("SHELL", "TERM", "CHERE_INVOKING", "PWD", "OLDPWD"):
        env.pop(k, None)
    # Match scripts/idf_build_flash_badge.ps1 — IDF python guard rejects MSYSTEM-style shells.
    for k in ("MSYSTEM", "MSYS2_ROOT", "MINGW_PREFIX", "MINGW_CHOST", "MINGW_PACKAGE_PREFIX"):
        env.pop(k, None)
    return env


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    default_project = repo_root / "platform" / "espidf" / "projects" / "dashcdg_badge"
    default_logs = repo_root / "docs" / "ops" / "logs" / "esp32-debug-cycle"

    p = argparse.ArgumentParser(description="Build/flash badge firmware and capture serial logs in a loop.")
    p.add_argument("--project-dir", default=str(default_project), help="Path to dashcdg_badge project directory.")
    p.add_argument("--log-dir", default=str(default_logs), help="Directory for captured UART logs.")
    if os.name == "nt":
        p.add_argument(
            "--build-dir",
            default=DEFAULT_BUILD_DIR_WIN,
            help=(
                "Out-of-tree build directory. Default is C:\\tmp\\dashcdg_badge_build to avoid OneDrive reparse-point "
                "restrictions in repo-local build/log (idf.py stdout/stderr capture files)."
            ),
        )
    else:
        p.add_argument("--build-dir", default="", help="Optional out-of-tree build directory.")
    p.add_argument("--port", default=DEFAULT_PORT, help="Serial port (default: COM6).")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="UART baud (default: 115200).")
    p.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS, help="Number of build/flash/log loops.")
    p.add_argument(
        "--log-seconds",
        type=int,
        default=DEFAULT_LOG_SECONDS,
        help="Seconds of UART capture per iteration.",
    )
    p.add_argument(
        "--idf-id",
        default=DEFAULT_IDF_ID,
        help="ESP-IDF installation ID used by idf_cmd_init / Initialize-Idf.",
    )
    p.add_argument(
        "--init-shell",
        choices=("cmd", "powershell"),
        default="powershell",
        help=(
            "ESP-IDF bootstrap: powershell (default) runs Initialize-Idf.ps1 then idf.py "
            "(Espressif desktop shortcut flow). "
            'cmd uses ""..\\idf_cmd_init.bat"" quoting (fragile if project path has spaces).'
        ),
    )
    p.add_argument("--skip-build-flash", action="store_true", help="Capture UART only; skip idf.py build flash.")
    p.add_argument(
        "--wait-dhcp-seconds",
        type=int,
        default=DEFAULT_WAIT_DHCP_SECONDS,
        help="How long to watch for STA DHCP log before reminder to launch CDG app.",
    )
    p.add_argument(
        "--min-lines",
        type=int,
        default=0,
        help="If >0, exit with code 2 when UART capture has fewer total lines (hint: idle UART or wrong COM port).",
    )
    return p.parse_args()


def run_build_flash(args: argparse.Namespace) -> None:
    project_dir = Path(args.project_dir).resolve()
    project_q = str(project_dir)
    build_dir = Path(args.build_dir).resolve() if args.build_dir else None
    build_q = str(build_dir) if build_dir else ""
    build_flag = f"-B \"{build_q}\" " if build_dir else ""
    if args.init_shell == "cmd":
        # Match Espressif shortcuts: cmd /k ""C:\Espressif\idf_cmd_init.bat" <IdfId>"
        # (Leading "" is cmd's escape so the batch path can be quoted.)
        cmd_line = (
            f'""C:\\Espressif\\idf_cmd_init.bat" {args.idf_id} '
            f'&& cd /d "{project_q}" '
            f'&& if not "{build_q}"=="" (mkdir "{build_q}" 2>nul) '
            f'&& idf.py {build_flag}-p {args.port} build flash'
        )
        cmd = ["C:\\Windows\\System32\\cmd.exe", "/c", cmd_line]
        print(f"[debug-cycle] build/flash (cmd):\n{cmd_line}\n")
        result = subprocess.run(cmd, env=windows_shell_env())
    else:
        ps_exe = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
        init_ps1 = r"C:\Espressif\Initialize-Idf.ps1"
        ps_cmd = (
            f"& '{init_ps1}' -IdfId '{args.idf_id}'; "
            f"Set-Location -LiteralPath '{project_q.replace(chr(39), chr(39) + chr(39))}'; "
            f"if ('{build_q}') {{ New-Item -ItemType Directory -Force -Path '{build_q}' | Out-Null }}; "
            f"idf.py {build_flag}-p {args.port} build flash"
        )
        cmd = [
            ps_exe,
            "-ExecutionPolicy",
            "Bypass",
            "-NoProfile",
            "-Command",
            ps_cmd,
        ]
        print(f"[debug-cycle] build/flash (powershell):\n{ps_cmd}\n")
        result = subprocess.run(cmd, env=windows_shell_env())

    if result.returncode != 0:
        raise RuntimeError(f"build/flash failed with exit code {result.returncode}")


def capture_uart(args: argparse.Namespace, log_file: Path) -> None:
    try:
        import serial  # type: ignore
    except Exception as exc:  # pragma: no cover - runtime environment dependent
        raise RuntimeError(
            "pyserial is required for UART capture. Install in ESP-IDF env: pip install pyserial"
        ) from exc

    print(f"[debug-cycle] capturing UART {args.port} @ {args.baud} for {args.log_seconds}s -> {log_file}")
    deadline = time.time() + float(args.log_seconds)
    total_lines = 0

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser, log_file.open("w", encoding="utf-8") as out:
        out.write(
            f"# debug-cycle timestamp={dt.datetime.now().isoformat()} port={args.port} baud={args.baud}\n"
        )
        dhcp_seen = False
        launch_seen = False
        dhcp_deadline = time.time() + float(max(1, args.wait_dhcp_seconds))
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode("utf-8", errors="replace")
            except Exception:
                line = repr(raw) + "\n"
            out.write(line)
            total_lines += 1
            if (not dhcp_seen) and ("sta ip:" in line.lower()):
                dhcp_seen = True
                print("[debug-cycle] DHCP seen on STA; launch CDG app now for playout test.")
            if (not launch_seen) and ("karaoke ui up" in line.lower()):
                launch_seen = True
                print("[debug-cycle] CDG app launch detected (karaoke UI up).")
            if (not dhcp_seen) and time.time() > dhcp_deadline:
                dhcp_deadline = float("inf")
                print("[debug-cycle] waiting on DHCP; once STA gets IP, launch CDG app manually.")
            # Keep terminal feedback lightweight.
            if total_lines % 50 == 0:
                print(f"[debug-cycle] ... {total_lines} lines")

    print(f"[debug-cycle] UART capture complete ({total_lines} lines)")
    if args.min_lines > 0 and total_lines < args.min_lines:
        print(
            f"[debug-cycle] WARNING: only {total_lines} lines captured (expected >= {args.min_lines}). "
            "Check USB cable, COM port, monitor baud, or that the badge is not stuck/deep sleep.",
            file=sys.stderr,
        )
        raise RuntimeError(f"UART capture too short: {total_lines} < {args.min_lines}")


def main() -> int:
    args = parse_args()
    project_dir = Path(args.project_dir).resolve()
    log_dir = Path(args.log_dir).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)

    if not project_dir.exists():
        raise RuntimeError(f"project directory not found: {project_dir}")

    for i in range(1, args.iterations + 1):
        tag = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        log_file = log_dir / f"esp32-cycle-{i:02d}-{tag}.log"
        print(f"\n[debug-cycle] iteration {i}/{args.iterations}")
        if not args.skip_build_flash:
            run_build_flash(args)
        capture_uart(args, log_file)

    print("\n[debug-cycle] all iterations complete")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[debug-cycle] interrupted")
        raise SystemExit(130)
    except Exception as exc:
        print(f"\n[debug-cycle] ERROR: {exc}", file=sys.stderr)
        if "UART capture too short" in str(exc):
            raise SystemExit(2)
        raise SystemExit(1)
