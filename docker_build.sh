#!/bin/sh
# SpotiBacan: compila la app, el servicio de fondo y el overlay en Docker (imagen smd-builder con devkitPro) y arma
# dist/SpotiBacan.zip listo para la raiz de la SD.
set -e
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/src -w /src smd-builder bash -c '
  set -e
  T=/opt/devkitpro/cmake/Switch.cmake
  cmake -S tools/cspot-app -B build_app -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.13 >/dev/null
  make -C build_app -j8 cspot bell 2>&1 | grep -E "error|Error" || true
  make -C app -j8 DEV=0 2>&1 | grep -E "error|Error|built" || true
  cmake -S phase2/sysmodule -B phase2/build_engine -G Ninja -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.13 >/dev/null
  cmake --build phase2/build_engine -j8 --target switify-background-package 2>&1 | grep -E " error|Error " || true
  make -C phase2/overlay -j8 TARGET=nxtify-overlay BUILD=build 2>&1 | grep -E " error|Error |built" || true
'
rm -rf dist && mkdir -p dist/sd/switch/SpotiBacan dist/sd/switch/.overlays dist/sd/atmosphere/contents/4200000000535754
cp app/SpotiBacan.nro dist/sd/switch/SpotiBacan/
cp phase2/overlay/nxtify-overlay.ovl dist/sd/switch/.overlays/SpotiBacan.ovl
cp phase2/build_engine/switify-background.nsp dist/sd/atmosphere/contents/4200000000535754/exefs.nsp
cp phase2/sysmodule/toolbox.json dist/sd/atmosphere/contents/4200000000535754/toolbox.json
(cd dist/sd && zip -qrX ../SpotiBacan.zip switch atmosphere)
ls -la dist/SpotiBacan.zip
