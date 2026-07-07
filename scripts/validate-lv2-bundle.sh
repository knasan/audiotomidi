#!/usr/bin/env bash
# Validate LV2 bundle metadata (URI registration) — run in MOD Docker after build.
#
# "can't get plugin" in mod-jackd = lilv cannot find the plugin URI (NOT dlopen).
# This script checks that lilv sees the URI before you deploy to hardware.
#
# Usage:
#   source /root/mod-plugin-builder/local.env moddwarf-new
#   ./scripts/validate-lv2-bundle.sh build-dwarf/audiotomidi.lv2

set -eo pipefail

BUNDLE="${1:-}"
URI="http://github.com/knasan/audiotomidi"

if [[ -z "${BUNDLE}" || ! -d "${BUNDLE}" ]]; then
    echo "Usage: $0 <path/to/audiotomidi.lv2>" >&2
    exit 1
fi

PARENT="$(cd "$(dirname "${BUNDLE}")" && pwd)"
NAME="$(basename "${BUNDLE}")"

echo "==> Bundle: ${BUNDLE}"
echo "==> Expected URI: ${URI}"
echo ""

for f in manifest.ttl audiotomidi.ttl modgui.ttl audiotomidi.so; do
    if [[ ! -e "${BUNDLE}/${f}" ]]; then
        echo "FAIL: missing ${f}" >&2
        exit 1
    fi
done
echo "OK: bundle files present"

if ! grep -q "${URI}" "${BUNDLE}/manifest.ttl" "${BUNDLE}/audiotomidi.ttl"; then
    echo "FAIL: URI not found in TTL files" >&2
    exit 1
fi
echo "OK: URI in manifest + audiotomidi.ttl"

if strings "${BUNDLE}/audiotomidi.so" | grep -q "${URI}"; then
    echo "OK: URI string in .so"
else
    echo "WARN: URI string not found in .so (descriptor mismatch risk)" >&2
fi

if command -v lilv-plugins >/dev/null 2>&1; then
    export LV2_PATH="${PARENT}"
    echo ""
    echo "==> lilv-plugins (LV2_PATH=${LV2_PATH}):"
    if lilv-plugins 2>/dev/null | grep -F "${URI}"; then
        echo "OK: lilv knows this plugin URI"
    else
        echo "FAIL: lilv does NOT list ${URI}" >&2
        echo "  TTL parse error or invalid manifest — mod-jackd will print: can't get plugin" >&2
        if command -v lv2validate >/dev/null 2>&1; then
            echo ""
            lv2validate "${BUNDLE}/" 2>&1 || true
        fi
        exit 1
    fi
else
    echo "NOTE: lilv-plugins not in PATH — skip lilv check"
fi

echo ""
echo "Deploy to Dwarf (registers bundle with running host):"
echo "  ./scripts/install-mod-dwarf.sh ${BUNDLE}"
