#!/usr/bin/env bash
# Full ABI report for audiotomidi.so — run in MOD Docker after build.
#
# Usage:
#   source /root/mod-plugin-builder/local.env moddwarf-new
#   ./scripts/diagnose-dwarf-binary.sh build-dwarf/audiotomidi.lv2/audiotomidi.so

set -eo pipefail

SO="${1:-}"
if [[ -z "${SO}" || ! -f "${SO}" ]]; then
    echo "Usage: $0 <path/to/audiotomidi.so>" >&2
    exit 1
fi

resolve_mod_tool() {
    local suffix="$1"
    local fallback="$2"
    if [[ -n "${CC:-}" && -x "${CC%-gcc}-${suffix}" ]]; then
        echo "${CC%-gcc}-${suffix}"
        return
    fi
    local candidate
    for candidate in \
        "/root/mod-workdir/moddwarf-new/host/usr/bin/aarch64-modaudio-linux-gnu-${suffix}" \
        "/root/mod-workdir/moddwarf/host/usr/bin/aarch64-mod-linux-gnu-${suffix}"; do
        if [[ -x "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done
    echo "${fallback}"
}

OBJDUMP="$(resolve_mod_tool objdump objdump)"
READELF="$(resolve_mod_tool readelf readelf)"

if [[ "${OBJDUMP}" == "objdump" ]]; then
    echo "WARN: using host objdump — source local.env moddwarf-new for accurate ARM output" >&2
    echo ""
fi

echo "========== FILE =========="
file "${SO}"
echo ""

echo "========== COMPILER COMMENT =========="
"${READELF}" -p .comment "${SO}" 2>/dev/null || true
echo ""

echo "========== NEEDED LIBRARIES =========="
"${READELF}" -d "${SO}" 2>/dev/null | grep NEEDED || true
echo ""

echo "========== GLIBC VERSIONS (imported) =========="
"${OBJDUMP}" -T "${SO}" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -u || true
echo ""

echo "========== GLIBCXX VERSIONS (imported) =========="
"${OBJDUMP}" -T "${SO}" 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sort -u || true
echo ""

echo "========== CXXABI VERSIONS (imported) =========="
"${OBJDUMP}" -T "${SO}" 2>/dev/null | grep -oE 'CXXABI_[0-9.]+' | sort -u || true
echo ""

echo "========== GLIBC_2.27 SYMBOLS (if any) =========="
"${OBJDUMP}" -T "${SO}" 2>/dev/null | grep 'GLIBC_2.27' | head -20 || echo "(none)"
echo ""

echo "========== C++ RUNTIME (UND without GLIBC — dlopen killers) =========="
"${OBJDUMP}" -T "${SO}" 2>/dev/null | grep ' UND ' | grep -v GLIBC | grep -E '_Z|pthread|ITM' | head -15 || echo "(none)"
echo ""

echo "========== LV2 EXPORT =========="
"${OBJDUMP}" -T "${SO}" 2>/dev/null | grep lv2_descriptor || echo "MISSING lv2_descriptor export!"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -x "${SCRIPT_DIR}/verify-dwarf-binary.sh" ]]; then
    echo "========== VERIFY =========="
    "${SCRIPT_DIR}/verify-dwarf-binary.sh" "${SO}" || true
fi

if [[ -x "${SCRIPT_DIR}/check-glibc-symbols.sh" ]]; then
    echo ""
    echo "========== GLIBC vs 2.17 (old firmware) =========="
    MAX_GLIBC=2.17 "${SCRIPT_DIR}/check-glibc-symbols.sh" "${SO}" || true
fi
