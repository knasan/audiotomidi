#!/usr/bin/env bash
# List symbols that require GLIBC newer than a given version.
#
# Usage (in MOD Docker):
#   source /root/mod-plugin-builder/local.env moddwarf-new
#   ./scripts/check-glibc-symbols.sh build-dwarf/audiotomidi.lv2/audiotomidi.so

set -eo pipefail

SO="${1:-}"
MAX_GLIBC="${MAX_GLIBC:-2.17}"

if [[ -z "${SO}" || ! -f "${SO}" ]]; then
    echo "Usage: MAX_GLIBC=2.17 $0 <audiotomidi.so>" >&2
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

echo "==> Object: ${SO}"
echo "==> Tool:   ${OBJDUMP}"
echo "==> Flag symbols newer than GLIBC_${MAX_GLIBC}"
echo ""

export MAX_GLIBC
"${OBJDUMP}" -T "${SO}" 2>/dev/null | python3 -c "
import os, re, sys

max_glibc = os.environ.get('MAX_GLIBC', '2.17')
max_parts = tuple(int(x) for x in max_glibc.split('.'))
# objdump -T format: ... (GLIBC_2.27) expf
pat = re.compile(r'\(GLIBC_(\d+\.\d+)\)\s+(\S+)')

flagged = []
for line in sys.stdin:
    for ver_num, sym in pat.findall(line):
        parts = tuple(int(x) for x in ver_num.split('.'))
        if parts > max_parts:
            flagged.append((f'GLIBC_{ver_num}', sym))

if not flagged:
    print(f'OK: no imported symbols above GLIBC_{max_glibc}')
    sys.exit(0)

print(f'FAIL: {len(flagged)} symbol(s) above GLIBC_{max_glibc}:')
for ver, sym in sorted(set(flagged)):
    print(f'  {ver}  {sym}')
print()
print('Fix for old Dwarf firmware (GLIBC_2.17 only):')
print('  MOD_GLIBC_217=1 ./scripts/build-mod-dwarf.sh')
print('Or update Dwarf firmware (provides GLIBC_2.27).')
sys.exit(1)
"
