#!/usr/bin/env python3
"""Build a portable source-build deb or package a native Linux binary."""

import argparse
import gzip
import io
import os
import tarfile
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]
VERSION = "1.5.0-1"


def tar_bytes(entries):
    raw = io.BytesIO()
    with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT) as archive:
            for name, content, mode in entries:
                info = tarfile.TarInfo(name)
                info.mode = mode
                info.uid = info.gid = 0
                info.mtime = 0
                if content is None:
                    info.type = tarfile.DIRTYPE
                    archive.addfile(info)
                else:
                    info.size = len(content)
                    archive.addfile(info, io.BytesIO(content))
    return raw.getvalue()


def ar_member(name, payload):
    title = (name + "/").ljust(16)
    header = f"{title}{0:<12}{0:<6}{0:<6}{0o100644:<8}{len(payload):<10}`\n".encode("ascii")
    assert len(header) == 60
    return header + payload + (b"\n" if len(payload) % 2 else b"")


def build(output, binary=None, architecture="all", runtime_dependencies=None):
    if binary is None and architecture != "all":
        raise ValueError("没有原生二进制时，架构必须为 all")
    if binary is not None and architecture == "all":
        raise ValueError("原生二进制必须指定 --arch")
    dependencies = (runtime_dependencies if binary and runtime_dependencies else
                    "libdrogon1t64, libssl3t64 | libssl3, libsqlite3-0, libstdc++6, adduser, systemd" if binary
                    else "cmake, g++, libdrogon-dev, libjsoncpp-dev, uuid-dev, zlib1g-dev, libssl-dev, libsqlite3-dev, libpq-dev, libmariadb-dev, libbrotli-dev, libhiredis-dev, libyaml-cpp-dev, adduser, systemd")
    control = f"""Package: myfolder-lan
Version: {VERSION}
Section: web
Priority: optional
Architecture: {architecture}
Depends: {dependencies}
Maintainer: MyFolder <local@myfolder.invalid>
Description: MyFolder LAN file server
 C++ intranet file manager with SQLite and path-based permissions.
""".encode()
    postinst = b"""#!/bin/sh
set -e
if ! id myfolder-lan >/dev/null 2>&1; then
    adduser --system --group --home /var/lib/myfolder-lan --no-create-home myfolder-lan
fi
install -d -m 0750 -o myfolder-lan -g myfolder-lan /var/lib/myfolder-lan /var/lib/myfolder-lan/files
if [ -f /opt/myfolder-lan/cpp/src/main.cpp ]; then
    install -d -m 0755 /opt/myfolder-lan/bin
    build_dir=$(mktemp -d)
    trap 'rm -rf "$build_dir"' EXIT
    cmake -S /opt/myfolder-lan/cpp -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG'
    cmake --build "$build_dir" --config Release --parallel 1
    install -m 0755 "$build_dir/myfolder-lan" /opt/myfolder-lan/bin/myfolder-lan
fi
systemctl daemon-reload
systemctl enable myfolder-lan.service >/dev/null
systemctl restart myfolder-lan.service
echo 'Create the first admin: sudo -u myfolder-lan env MYFOLDER_DATA=/var/lib/myfolder-lan /opt/myfolder-lan/bin/myfolder-lan --init-admin'
"""
    prerm = b"""#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = deconfigure ]; then
    systemctl stop myfolder-lan.service || true
fi
"""
    postrm = b"""#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    rm -f /opt/myfolder-lan/bin/myfolder-lan
fi
systemctl daemon-reload || true
"""
    service = b"""[Unit]
Description=MyFolder LAN file server
After=network.target

[Service]
Type=simple
User=myfolder-lan
Group=myfolder-lan
Environment=MYFOLDER_DATA=/var/lib/myfolder-lan
Environment=MYFOLDER_WEB=/opt/myfolder-lan/web/dist
EnvironmentFile=-/etc/default/myfolder-lan
ExecStart=/opt/myfolder-lan/bin/myfolder-lan
WorkingDirectory=/var/lib/myfolder-lan
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/myfolder-lan
PrivateTmp=true

[Install]
WantedBy=multi-user.target
"""
    defaults = b"""# Bind to a LAN interface or change the port if required.
MYFOLDER_HOST=0.0.0.0
MYFOLDER_PORT=8080
MYFOLDER_MAX_UPLOADS=2
MYFOLDER_IO_WORKERS=4
MYFOLDER_MAX_REQUEST_GIB=5
"""
    control_tar = tar_bytes([
        ("./control", control, 0o644),
        ("./conffiles", b"/etc/default/myfolder-lan\n", 0o644),
        ("./postinst", postinst, 0o755),
        ("./prerm", prerm, 0o755),
        ("./postrm", postrm, 0o755),
    ])
    files = [
        ("./lib/systemd/system/myfolder-lan.service", service, 0o644),
        ("./etc/default/myfolder-lan", defaults, 0o644),
    ]
    if binary:
        files.append(("./opt/myfolder-lan/bin/myfolder-lan", Path(binary).read_bytes(), 0o755))
    else:
        for relative in ("CMakeLists.txt", "src/main.cpp", "third_party/httplib.h", "third_party/httplib.LICENSE",
                         "third_party/json.hpp", "third_party/json.LICENSE"):
            files.append((f"./opt/myfolder-lan/cpp/{relative}", (BASE / "cpp" / relative).read_bytes(), 0o644))
    web_dist = BASE / "web" / "dist"
    if not (web_dist / "index.html").is_file():
        raise FileNotFoundError("Vue Web 构建产物不存在，请先运行 cd web && npm run build")
    for asset in sorted(web_dist.rglob("*")):
        if asset.is_file():
            relative = asset.relative_to(web_dist).as_posix()
            files.append((f"./opt/myfolder-lan/web/dist/{relative}", asset.read_bytes(), 0o644))
    directories = set()
    for name, _, _ in files:
        parts = name.removeprefix("./").split("/")[:-1]
        for count in range(1, len(parts) + 1):
            directories.add("./" + "/".join(parts[:count]) + "/")
    data_tar = tar_bytes([(name, None, 0o755) for name in sorted(directories, key=lambda value: (value.count("/"), value))] + files)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"!<arch>\n" + ar_member("debian-binary", b"2.0\n")
                       + ar_member("control.tar.gz", control_tar)
                       + ar_member("data.tar.gz", data_tar))
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=BASE / "dist" / f"myfolder-lan_{VERSION}_all.deb")
    parser.add_argument("--binary", type=Path, help="Linux ELF binary produced on the target distro/architecture")
    parser.add_argument("--arch", default="all", help="Debian architecture for --binary, e.g. arm64 or amd64")
    parser.add_argument("--runtime-depends", help="Debian Depends for a target-built ELF with custom static libraries")
    args = parser.parse_args()
    build(args.output, args.binary, args.arch, args.runtime_depends)
