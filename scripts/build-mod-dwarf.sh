#!/usr/bin/env bash
# Cross-build audiotomidi.lv2 for MOD Dwarf using the official mod-plugin-builder
# toolchain (correct sysroot + glibc), NOT Debian's aarch64-linux-gnu-gcc.
#
# Docker (recommended):
#   docker run --rm -ti -v "$(pwd)":/root/source mpb-moddwarf-new:latest bash
#   cd /root/source && ./scripts/build-mod-dwarf.sh
#
# Or source once manually, then build (script detects existing env):
#   source /root/mod-plugin-builder/local.env moddwarf-new
#   ./scripts/build-mod-dwarf.sh
#
# Deploy:
#   scp -r build-dwarf/audiotomidi.lv2 root@mod-dwarf.local:/root/.lv2/

# Note: no "set -u" — mod-plugin-builder local.env line 87 uses unbound ${ld}.
set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MPB="${MOD_PLUGIN_BUILDER:-${HOME}/mod-plugin-builder}"
PLATFORM="${MOD_PLATFORM:-moddwarf-new}"
BUILD="${ROOT}/build-dwarf"
VERIFY="${ROOT}/scripts/verify-dwarf-binary.sh"

mod_env_ready() {
    [[ -n "${HOST_DIR:-}" && -n "${STAGING_DIR:-}" && -n "${CC:-}" ]] \
        && { [[ "${CC}" == *"-mod"* ]] || [[ "${CC}" == *"modaudio"* ]]; }
}

if mod_env_ready; then
    echo "==> Using existing MOD cross-compile environment"
else
    if [[ ! -f "${MPB}/local.env" ]]; then
        echo "ERROR: mod-plugin-builder not found." >&2
        echo "  Expected: ${MPB}/local.env" >&2
        echo "  Or run: source ${MPB}/local.env ${PLATFORM}" >&2
        exit 1
    fi
    # shellcheck disable=SC1091
    source "${MPB}/local.env" "${PLATFORM}"
fi

TOOLCHAIN_FILE="${HOST_DIR}/usr/share/mod-plugin-builder/toolchainfile.cmake"
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    echo "ERROR: MOD toolchain file missing: ${TOOLCHAIN_FILE}" >&2
    echo "  Platform '${PLATFORM}' may not be bootstrapped in this image." >&2
    echo "  This Docker image likely only has moddwarf-new." >&2
    echo "  Use: MOD_PLATFORM=moddwarf-new ./scripts/build-mod-dwarf.sh" >&2
    echo "  Or rebuild Docker: docker build --build-arg platform=moddwarf ..." >&2
    exit 1
fi

if [[ "${CC}" != *"-mod"* && "${CC}" != *"modaudio"* ]]; then
    echo "ERROR: Wrong cross-compiler: ${CC}" >&2
    echo "  Expected aarch64-mod-linux-gnu-gcc or aarch64-modaudio-linux-gnu-gcc." >&2
    exit 1
fi

echo "==> MOD platform:  ${PLATFORM}"
echo "==> Compiler:      ${CC}"
echo "==> Sysroot:       ${STAGING_DIR}"
echo "==> Toolchain:     ${TOOLCHAIN_FILE}"
if [[ "${MOD_GLIBC_217:-0}" == "1" ]]; then
    echo "==> GLIBC target:  2.17 (old Dwarf firmware)"
    GLIBC_CMAKE_FLAG="-DAUDIOTOMIDI_GLIBC_217=ON"
else
    echo "==> GLIBC target:  2.27 (moddwarf-new default)"
    GLIBC_CMAKE_FLAG=""
fi
echo ""

export CXXFLAGS="${CXXFLAGS:-} -fno-sized-deallocation"
rm -rf "${BUILD}"

cmake -S "${ROOT}" -B "${BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DAUDIOTOMIDI_BUILD_TESTS=OFF \
    ${GLIBC_CMAKE_FLAG}

# local.env aliases cmake for configure; use the real binary for --build.
/usr/bin/cmake --build "${BUILD}" --target audiotomidi_lv2 -j"$(nproc)"

SO="${BUILD}/audiotomidi.lv2/audiotomidi.so"
OBJDUMP="${CC%-gcc}-objdump"
echo ""
echo "Bundle: ${BUILD}/audiotomidi.lv2"
file "${SO}"

echo ""
echo "==> C++ undefined symbols (should be empty or weak-only):"
if [[ -x "${OBJDUMP}" ]]; then
    if "${OBJDUMP}" -T "${SO}" 2>/dev/null | grep ' UND ' | grep '_Z' | grep -qv ' w '; then
        echo "WARN: strong undefined C++ symbols detected:" >&2
        "${OBJDUMP}" -T "${SO}" 2>/dev/null | grep ' UND ' | grep '_Z' | grep -v ' w ' >&2 || true
    elif "${OBJDUMP}" -T "${SO}" 2>/dev/null | grep -q '_ZGTtnam'; then
        echo "NOTE: _ZGTtnam is weak — usually OK; if load fails, report in issue."
        "${OBJDUMP}" -T "${SO}" 2>/dev/null | grep '_ZGTtnam' || true
    else
        echo "OK: no _ZGTtnam"
    fi
fi

if [[ -x "${VERIFY}" ]]; then
    echo ""
    "${VERIFY}" "${SO}" || true
fi

DIAGNOSE="${ROOT}/scripts/diagnose-dwarf-binary.sh"
if [[ -x "${DIAGNOSE}" ]]; then
    echo ""
    "${DIAGNOSE}" "${SO}"
fi

CHECK_GLIBC="${ROOT}/scripts/check-glibc-symbols.sh"
if [[ -x "${CHECK_GLIBC}" ]]; then
    echo ""
    MAX_GLIBC="${MAX_GLIBC:-2.17}" "${CHECK_GLIBC}" "${SO}" || true
fi
