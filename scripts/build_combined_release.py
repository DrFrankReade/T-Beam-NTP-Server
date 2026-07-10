from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = Path.home() / ".platformio" / "build" / "tbeam_ntp_server" / "ttgo-t-beam"
PLATFORMIO_DIR = Path.home() / ".platformio"


def firmware_version() -> str:
    main_cpp = PROJECT_ROOT / "src" / "main.cpp"
    for line in main_cpp.read_text().splitlines():
        if "FIRMWARE_VERSION" in line and '"' in line:
            return line.split('"')[1]
    raise RuntimeError("FIRMWARE_VERSION not found in src/main.cpp")


def required_file(path: Path) -> Path:
    if not path.exists():
        raise FileNotFoundError(path)
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def merge_combined_binary(output: Path) -> None:
    tool_dir = required_file(PLATFORMIO_DIR / "packages" / "tool-esptoolpy")
    esptool = required_file(tool_dir / "esptool.py")
    python = required_file(PLATFORMIO_DIR / "penv" / "Scripts" / "python.exe")
    boot_app = required_file(
        PLATFORMIO_DIR
        / "packages"
        / "framework-arduinoespressif32"
        / "tools"
        / "partitions"
        / "boot_app0.bin"
    )

    parts = [
        ("0x1000", required_file(BUILD_DIR / "bootloader.bin")),
        ("0x8000", required_file(BUILD_DIR / "partitions.bin")),
        ("0xe000", boot_app),
        ("0x10000", required_file(BUILD_DIR / "firmware.bin")),
        ("0x300000", required_file(BUILD_DIR / "littlefs.bin")),
    ]

    output.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PYTHONPATH"] = str(tool_dir)
    command = [
        str(python),
        str(esptool),
        "--chip",
        "esp32",
        "merge_bin",
        "--flash_mode",
        "qio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "4MB",
        "-o",
        str(output),
    ]
    for offset, path in parts:
        command.extend([offset, str(path)])

    subprocess.run(command, check=True, cwd=PROJECT_ROOT, env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the single-file ESP32 release image.")
    parser.add_argument("--version", default=firmware_version(), help="Release version string, default: firmware version")
    parser.add_argument("--output-dir", default=PROJECT_ROOT / "release", type=Path)
    args = parser.parse_args()

    output = args.output_dir / f"t-beam-ntp-server-{args.version}-combined.bin"
    merge_combined_binary(output)
    print(output)
    print(f"size={output.stat().st_size}")
    print(f"sha256={sha256(output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
