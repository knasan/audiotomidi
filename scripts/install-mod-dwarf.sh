#!/usr/bin/env bash
# Install audiotomidi.lv2 on MOD Dwarf via the SDK API (preferred over raw scp).
# Ensures the running mod-host/mod-jackd reloads the LV2 bundle.
#
# Usage:
#   ./scripts/install-mod-dwarf.sh
#   ./scripts/install-mod-dwarf.sh build-dwarf/audiotomidi.lv2
#   MOD_IP=192.168.51.1 ./scripts/install-mod-dwarf.sh

set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="${1:-${ROOT}/build-dwarf/audiotomidi.lv2}"
MOD_IP="${MOD_IP:-mod-dwarf.local}"

if [[ ! -d "${BUNDLE}" ]]; then
    echo "ERROR: bundle not found: ${BUNDLE}" >&2
    echo "Build first: ./scripts/build-mod-dwarf.sh" >&2
    exit 1
fi

echo "==> Installing ${BUNDLE} to ${MOD_IP} via MOD SDK..."
echo "    (tar | base64 | curl /sdk/install)"

TMP="$(mktemp)"
tar -C "$(dirname "${BUNDLE}")" -czf "${TMP}" "$(basename "${BUNDLE}")"
base64 "${TMP}" | curl -sf -F "package=@-" "http://${MOD_IP}/sdk/install"
rm -f "${TMP}"

echo ""
echo "OK: install request sent."
echo "Wait a few seconds, then add the plugin to a NEW pedalboard."
echo "If it still fails, reboot the Dwarf (mod-jackd must reload LV2 world)."
