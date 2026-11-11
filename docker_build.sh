#!/bin/sh
# SpotiBacan: compila la app en Docker (imagen smd-builder con devkitPro).
set -e
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/src -w /src smd-builder bash -c '
  set -e
  cmake -S tools/cspot-app -B build_app -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.13 >/dev/null
  make -C build_app -j8 cspot bell 2>&1 | grep -E "error|Error|Built target" || true
  make -C app -j8 DEV=0 2>&1 | grep -E "error|Error|built|linking" || true
'
ls -la app/*.nro
