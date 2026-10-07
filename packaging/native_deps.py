#!/usr/bin/env python3
"""Report Debian runtime Depends for a native ELF using dpkg-shlibdeps."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        control = root / "debian" / "control"
        control.parent.mkdir()
        control.write_text(
            "Source: myfolder-lan\nSection: web\nPriority: optional\n"
            "Maintainer: MyFolder <local@myfolder.invalid>\n\n"
            "Package: myfolder-lan\nArchitecture: any\n"
            "Depends: ${shlibs:Depends}\nDescription: MyFolder LAN server\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            ["dpkg-shlibdeps", "-O", "-e" + str(binary)],
            cwd=root, capture_output=True, text=True, check=True,
        )
    line = next((item for item in result.stdout.splitlines()
                 if item.startswith("shlibs:Depends=")), None)
    if line is None:
        raise SystemExit("dpkg-shlibdeps did not report runtime dependencies")
    print(line.partition("=")[2] + ", adduser, systemd")


if __name__ == "__main__":
    main()
