#!/bin/sh
# Uploads the firmware the last `idf.py build` made to IoT Support, by running the upload script
# IoT Support itself serves: that script collects the build's files and posts them. Jenkins'
# espFirmware.upload runs this with IoT Support's base URL. The repository is public, so there is
# no default address.

set -e

cd "$(dirname "$0")/.."

HOST=${1:?usage: scripts/upload.sh <IoT Support base URL>}

curl -fsSL "$HOST/api/pipeline/upload.sh" | sh -s --
