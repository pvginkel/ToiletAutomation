#!/bin/sh
# Builds the two spike firmwares for esp32s3 into build/, in the esp-idf tool container:
# ESP-IDF's own hello_world, built out of tree from $IDF_PATH, and the abort variant.
set -eu
cd "$(dirname "$0")"
here=$(pwd)
exec cexec esp-idf sh -c "
  set -eu
  cd '$here'
  idf.py -C \"\$IDF_PATH/examples/get-started/hello_world\" -B build/hello_world \
    -DSDKCONFIG='$here/build/hello_world/sdkconfig' -DIDF_TARGET=esp32s3 build
  idf.py -C firmware/abort -B build/abort \
    -DSDKCONFIG='$here/build/abort/sdkconfig' -DIDF_TARGET=esp32s3 build
  # The console example pulls a managed component, whose lock file is written next to the
  # source; the ESP-IDF tree is read-only, so build a copy.
  rm -rf build/console-src && mkdir -p build/console-src
  cp -r \"\$IDF_PATH/examples/system/console/basic/.\" build/console-src/
  idf.py -C build/console-src -B build/console \
    -DSDKCONFIG='$here/build/console/sdkconfig' -DSDKCONFIG_DEFAULTS='$here/firmware/console.defaults' \
    -DIDF_TARGET=esp32s3 build
  idf.py -C \"\$IDF_PATH/examples/get-started/hello_world\" -B build/hello_world_esp32 \
    -DSDKCONFIG='$here/build/hello_world_esp32/sdkconfig' -DIDF_TARGET=esp32 build
"
