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
"
