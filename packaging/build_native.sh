#!/usr/bin/env bash
set -euo pipefail

suite="${1:?Usage: packaging/build_native.sh ubuntu22.04|ubuntu24.04|debian12|debian13}"
case "$suite" in
  ubuntu22.04|ubuntu24.04|debian12|debian13) ;;
  *) echo "Unsupported target: $suite" >&2; exit 2 ;;
esac

project_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_root"
test -f web/dist/index.html || { echo "Build web/dist first" >&2; exit 2; }
architecture="$(dpkg --print-architecture)"
version="$(python3 -c 'import sys; sys.path.insert(0, "packaging"); import build_deb; print(build_deb.VERSION)')"
build_root="$project_root/.build/native-${suite}-${architecture}"
mkdir -p "$build_root"

curl --fail --location --retry 5 --output "$build_root/drogon.tar.gz" \
  https://github.com/drogonframework/drogon/archive/refs/tags/v1.9.12.tar.gz
curl --fail --location --retry 5 --output "$build_root/trantor.tar.gz" \
  https://github.com/an-tao/trantor/archive/refs/tags/v1.5.26.tar.gz
tar -xzf "$build_root/drogon.tar.gz" -C "$build_root"
tar -xzf "$build_root/trantor.tar.gz" -C "$build_root"
cp -a "$build_root/trantor-1.5.26/." "$build_root/drogon-1.9.12/trantor/"

cmake -S "$build_root/drogon-1.9.12" -B "$build_root/drogon-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG' \
  -DCMAKE_INSTALL_PREFIX="$build_root/deps" -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_CTL=OFF -DBUILD_EXAMPLES=OFF -DBUILD_ORM=OFF \
  -DBUILD_TESTING=OFF -DBUILD_BROTLI=OFF -DBUILD_YAML_CONFIG=OFF
cmake --build "$build_root/drogon-build" --parallel "${MYFOLDER_BUILD_JOBS:-2}"
cmake --install "$build_root/drogon-build"

cmake -S cpp -B "$build_root/app-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG' \
  -DCMAKE_PREFIX_PATH="$build_root/deps"
cmake --build "$build_root/app-build" --parallel "${MYFOLDER_BUILD_JOBS:-2}"
binary="$build_root/app-build/myfolder-lan"
MYFOLDER_CPP_BINARY="$binary" python3 -m unittest discover -s tests -p 'test_*.py' -v

dependencies="$(python3 packaging/native_deps.py "$binary")"
output="dist/myfolder-lan_${version}_${suite}_${architecture}.deb"
python3 packaging/build_deb.py --binary "$binary" --arch "$architecture" \
  --runtime-depends "$dependencies" --output "$output"
dpkg-deb --field "$output" Package Version Architecture Depends
echo "Built $output"
