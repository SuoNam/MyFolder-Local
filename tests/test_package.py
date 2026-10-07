import io
import tarfile
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
import build_deb


class PackageTest(unittest.TestCase):
    def test_debian_archive_contains_service_and_web(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "myfolder-lan.deb"
            build_deb.build(package)
            payload = package.read_bytes()
            self.assertTrue(payload.startswith(b"!<arch>\n"))
            pos = 8
            members = {}
            while pos < len(payload):
                header = payload[pos:pos + 60]
                name = header[:16].strip().rstrip(b"/").decode()
                size = int(header[48:58].strip())
                members[name] = payload[pos + 60:pos + 60 + size]
                pos += 60 + size + size % 2
            self.assertEqual(members["debian-binary"], b"2.0\n")
            with tarfile.open(fileobj=io.BytesIO(members["control.tar.gz"]), mode="r:gz") as archive:
                names = archive.getnames()
                self.assertIn("./control", names)
                self.assertIn("./conffiles", names)
                self.assertIn("./postinst", names)
                control = archive.extractfile("./control").read()
                self.assertIn(b"g++, libdrogon-dev", control)
                self.assertIn(b"Architecture: all", control)
                postinst = archive.extractfile("./postinst").read()
                self.assertIn(b"cmake --build", postinst)
            with tarfile.open(fileobj=io.BytesIO(members["data.tar.gz"]), mode="r:gz") as archive:
                names = archive.getnames()
                self.assertTrue(archive.getmember("./opt/myfolder-lan/cpp/").isdir())
                self.assertTrue(archive.getmember("./opt/myfolder-lan/web/dist/").isdir())
                self.assertIn("./opt/myfolder-lan/cpp/src/main.cpp", names)
                self.assertIn("./opt/myfolder-lan/cpp/CMakeLists.txt", names)
                self.assertIn("./opt/myfolder-lan/cpp/third_party/httplib.h", names)
                self.assertIn("./opt/myfolder-lan/cpp/third_party/json.hpp", names)
                self.assertIn("./opt/myfolder-lan/web/dist/index.html", names)
                self.assertTrue(any(name.startswith("./opt/myfolder-lan/web/dist/assets/") and name.endswith(".js") for name in names))
                self.assertTrue(any(name.startswith("./opt/myfolder-lan/web/dist/assets/") and name.endswith(".css") for name in names))
                self.assertIn("./lib/systemd/system/myfolder-lan.service", names)
                service = archive.extractfile("./lib/systemd/system/myfolder-lan.service").read()
                self.assertIn(b"ExecStart=/opt/myfolder-lan/bin/myfolder-lan", service)

    def test_native_archive_creates_binary_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "myfolder-lan"
            binary.write_bytes(b"\x7fELF")
            package = Path(directory) / "native.deb"
            build_deb.build(package, binary=binary, architecture="arm64",
                            runtime_dependencies="libssl3, systemd")
            payload = package.read_bytes()
            pos = 8
            members = {}
            while pos < len(payload):
                header = payload[pos:pos + 60]
                name = header[:16].strip().rstrip(b"/").decode()
                size = int(header[48:58].strip())
                members[name] = payload[pos + 60:pos + 60 + size]
                pos += 60 + size + size % 2
            with tarfile.open(fileobj=io.BytesIO(members["data.tar.gz"]), mode="r:gz") as archive:
                self.assertTrue(archive.getmember("./opt/myfolder-lan/bin/").isdir())
                self.assertEqual(archive.extractfile("./opt/myfolder-lan/bin/myfolder-lan").read(), b"\x7fELF")


if __name__ == "__main__":
    unittest.main()
