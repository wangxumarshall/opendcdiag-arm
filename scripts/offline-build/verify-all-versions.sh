#!/bin/bash
# verify-all-versions.sh — drive docker-build-test.sh across all 15 openEuler
# LTS versions (reverse: 24.03 -> 22.03 -> 20.03) and collect a results table.
#
# Each version: offline RPM install -> meson setup (cxx_compat auto) -> ninja
# -> list-tests -> zstd19 + a couple regression tests. Captures real output.
#
# Usage: ./verify-all-versions.sh [pattern]
#   pattern  optional grep -E pattern to filter versions (e.g. '24.03' )
set -uo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

REPO="$PWD"
RESULTS="$REPO/build-results"
mkdir -p "$RESULTS"

# (image-tag, rpm-series) pairs, reverse order
ALL=(
    "24.03-LTS-SP4|openEuler-24.03"
    "24.03-LTS-SP3|openEuler-24.03"
    "24.03-LTS-SP2|openEuler-24.03"
    "24.03-LTS-SP1|openEuler-24.03"
    "24.03-LTS|openEuler-24.03"
    "22.03-LTS-SP4|openEuler-22.03"
    "22.03-LTS-SP3|openEuler-22.03"
    "22.03-LTS-SP2|openEuler-22.03"
    "22.03-LTS-SP1|openEuler-22.03"
    "22.03-LTS|openEuler-22.03"
    "20.03-LTS-SP4|openEuler-20.03"
    "20.03-LTS-SP3|openEuler-20.03"
    "20.03-LTS-SP2|openEuler-20.03"
    "20.03-LTS-SP1|openEuler-20.03"
    "20.03-LTS|openEuler-20.03"
)

PAT="${1:-.}"
printf "%-16s | %-10s | %-6s | %-8s | %-10s | %s\n" "VERSION" "DEPS" "BUILD" "TESTS" "ZSTD19" "NOTE"
printf -- "-----------------+------------+--------+----------+------------+-------------------\n"

for entry in "${ALL[@]}"; do
    tag="${entry%%|*}"; series="${entry##*|}"
    echo "$tag" | grep -qE "$PAT" || continue
    log="$RESULTS/${tag}.log"
    note=""
    if ! podman image inspect "openeuler-offline:${tag}" >/dev/null 2>&1; then
        printf "%-16s | %-10s | %-6s | %-8s | %-10s | %s\n" "$tag" "-" "-" "-" "-" "image not loaded"
        continue
    fi
    # Run the build+test; capture full log, tolerate failure (one bad version
    # must not abort the whole sweep).
    "$REPO/scripts/offline-build/docker-build-test.sh" "$tag" "$series" > "$log" 2>&1
    rc=$?
    # Extract status from log
    deps=$(grep -cE "安装完成" "$log" 2>/dev/null | head -1); [ "$deps" = "" ] && deps="?"
    if grep -q "完成。二进制" "$log" 2>/dev/null; then build="OK"; else build="FAIL"; fi
    ntests=$(grep -oE "list-tests 行数: [0-9]+" "$log" 2>/dev/null | grep -oE "[0-9]+$" | head -1); [ -z "$ntests" ] && ntests="-"
    zstd=$(grep -iE "zstd19" "$log" 2>/dev/null | grep -ioE "exit: (pass|fail)" | head -1); [ -z "$zstd" ] && zstd="-"
    if [ "$rc" -ne 0 ] && [ "$build" != "OK" ]; then
        # capture first error line as note
        note=$(grep -iE "error:" "$log" 2>/dev/null | head -1 | cut -c1-50)
        [ -z "$note" ] && note="rc=$rc"
    fi
    printf "%-16s | %-10s | %-6s | %-8s | %-10s | %s\n" "$tag" "$deps" "$build" "$ntests" "$zstd" "$note"
done

echo ""
echo "Full logs in $RESULTS/<version>.log"
