#!/usr/bin/env bash
# Verify audiotomidi.so is loadable on MOD Dwarf (AArch64 + compatible glibc).
#
# Usage:
#   ./scripts/verify-dwarf-binary.sh build-dwarf/audiotomidi.lv2/audiotomidi.so
#
# Optional:
#   MAX_GLIBC=2.27   # default; set 2.17 only for very old Dwarf firmware

set -euo pipefail

SO="${1:-}"
MAX_GLIBC="${MAX_GLIBC:-2.27}"

if [[ -z "${SO}" || ! -f "${SO}" ]]; then
    echo "Usage: $0 <path/to/audiotomidi.so>" >&2
    exit 1
fi

echo "==> Verifying: ${SO}"

# Architecture
if ! file "${SO}" | grep -q 'ELF 64-bit.*ARM aarch64'; then
    echo "FAIL: not AArch64 ELF" >&2
    file "${SO}" >&2
    exit 1
fi
echo "OK:   AArch64 ELF"

# Pick readelf/objdump from MOD toolchain when available.
READELF="${READELF:-}"
if [[ -z "${READELF}" && -n "${CC:-}" ]]; then
    READELF="${CC%-gcc}-readelf"
fi
if [[ -z "${READELF}" || ! -x "${READELF}" ]]; then
    READELF="readelf"
fi
OBJDUMP="${OBJDUMP:-objdump}"

# Imported glibc symbol versions (undefined symbols tagged with GLIBC_x.y).
GLIBC_VERSIONS="$("${OBJDUMP}" -T "${SO}" 2>/dev/null \
    | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
    | sort -u || true)"

if [[ -z "${GLIBC_VERSIONS}" ]]; then
    echo "WARN: no GLIBC version tags found in dynamic symbol table"
else
    echo "GLIBC imports:"
    echo "${GLIBC_VERSIONS}" | sed 's/^/  /'

    TOO_NEW="$(
        echo "${GLIBC_VERSIONS}" \
        | sed 's/GLIBC_//' \
        | MAX_GLIBC="${MAX_GLIBC}" python3 -c "
import os, sys
max_v = tuple(int(x) for x in os.environ['MAX_GLIBC'].split('.'))
for line in sys.stdin:
    v = line.strip()
    if not v:
        continue
    parts = tuple(int(x) for x in v.split('.'))
    if parts > max_v:
        print(v)
"
    )"

    if [[ -n "${TOO_NEW}" ]]; then
        echo "FAIL: glibc newer than ${MAX_GLIBC}:" >&2
        echo "${TOO_NEW}" | sed 's/^/  GLIBC_/' >&2
        echo "  Rebuild with ./scripts/build-mod-dwarf.sh (MOD toolchain), not build-dwarf-real.sh." >&2
        exit 1
    fi
    echo "OK:   glibc <= ${MAX_GLIBC}"
fi

# Quick sanity: Debian cross-build often leaves these at GLIBC_2.29+.
if "${OBJDUMP}" -T "${SO}" 2>/dev/null | grep -qE 'GLIBC_2\.(2[89]|[3-9])'; then
    echo "FAIL: GLIBC_2.28+ detected — typical sign of Debian aarch64-linux-gnu-gcc build." >&2
    exit 1
fi

# GLIBCXX from host libstdc++ is another common MOD load failure.
if "${OBJDUMP}" -T "${SO}" 2>/dev/null | grep -qE 'GLIBCXX_3\.4\.(2[89]|[3-9])'; then
    echo "FAIL: GLIBCXX_3.4.28+ detected — rebuild with -static-libstdc++ (MOD cross-build)." >&2
    exit 1
fi

if "${READELF}" -d "${SO}" 2>/dev/null | grep -q 'libstdc++'; then
    echo "WARN: links libstdc++.so dynamically — prefer -static-libstdc++ for MOD Dwarf." >&2
fi

echo "OK:   binary looks Dwarf-compatible"
