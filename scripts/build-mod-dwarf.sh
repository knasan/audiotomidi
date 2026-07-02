#!/usr/bin/env bash
# Cross-build audiotomidi.lv2 for MOD Dwarf (aarch64) using mod-plugin-builder.
#
# Prerequisites:
#   git clone https://github.com/moddevices/mod-plugin-builder.git
#   cd mod-plugin-builder && make
#
# Usage (adjust MOD_PLUGIN_BUILDER to your checkout):
#   MOD_PLUGIN_BUILDER=~/mod-plugin-builder ./scripts/build-mod-dwarf.sh
#
# Deploy to a connected Dwarf (USB / network):
#   scp -r build-dwarf/audiotomidi.lv2 root@mod-dwarf.local:/root/.lv2/

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MPB="${MOD_PLUGIN_BUILDER:-${HOME}/mod-plugin-builder}"
BUILD="${ROOT}/build-dwarf"

if [[ ! -d "${MPB}/toolchain" ]]; then
    echo "mod-plugin-builder not found at: ${MPB}" >&2
    echo "Set MOD_PLUGIN_BUILDER or clone https://github.com/moddevices/mod-plugin-builder" >&2
    exit 1
fi

# shellcheck disable=SC1091
source "${MPB}/local.env"

cmake -S "${ROOT}" -B "${BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${MPB}/toolchain/moddwarf.cmake" \
    -DAUDIOTOMIDI_BUILD_TESTS=OFF

cmake --build "${BUILD}" --target audiotomidi_lv2 -j"$(nproc)"

echo ""
echo "Bundle: ${BUILD}/audiotomidi.lv2"
file "${BUILD}/audiotomidi.lv2/audiotomidi.so"
