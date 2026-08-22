#!/bin/bash
# supplement-gmp-devel.sh — back-fill the missing gmp-devel + gmp-c++ RPMs
# into all 15 LTS trees under third-party/rpms/.
#
# Context: the original download-deps.sh / download-all-versions.sh collected
# gcc's dependency tree via `dnf download --resolve --alldeps`, but gmp-devel
# (which provides /usr/include/gmp.h, required by the arithmetic_arm tests)
# was not in gcc's resolved requires (gcc links libgmp.so at runtime, does not
# need gmp.h itself). gmp-devel in turn requires gmp-c++ (a separate subpackage
# with epoch 1). So every tree shipped gmp (runtime) but neither gmp-devel nor
# gmp-c++. This script back-fills both into each existing tree so the source
# compiles without a full re-download.
#
# Source: https://repo.openeuler.org/openEuler-<ver>/OS/aarch64/Packages/
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

REPO="https://repo.openeuler.org"

# (repo-version-dir, tree-subdir)
ENTRIES=(
    "openEuler-24.03-LTS-SP4|openEuler-24.03/openEuler-24.03LTS_SP4"
    "openEuler-24.03-LTS-SP3|openEuler-24.03/openEuler-24.03LTS_SP3"
    "openEuler-24.03-LTS-SP2|openEuler-24.03/openEuler-24.03LTS_SP2"
    "openEuler-24.03-LTS-SP1|openEuler-24.03/openEuler-24.03LTS_SP1"
    "openEuler-24.03-LTS|openEuler-24.03/openEuler-24.03LTS"
    "openEuler-22.03-LTS-SP4|openEuler-22.03/openEuler-22.03LTS_SP4"
    "openEuler-22.03-LTS-SP3|openEuler-22.03/openEuler-22.03LTS_SP3"
    "openEuler-22.03-LTS-SP2|openEuler-22.03/openEuler-22.03LTS_SP2"
    "openEuler-22.03-LTS-SP1|openEuler-22.03/openEuler-22.03LTS_SP1"
    "openEuler-22.03-LTS|openEuler-22.03/openEuler-22.03LTS"
    "openEuler-20.03-LTS-SP4|openEuler-20.03/openEuler-20.03LTS_SP4"
    "openEuler-20.03-LTS-SP3|openEuler-20.03/openEuler-20.03LTS_SP3"
    "openEuler-20.03-LTS-SP2|openEuler-20.03/openEuler-20.03LTS_SP2"
    "openEuler-20.03-LTS-SP1|openEuler-20.03/openEuler-20.03LTS_SP1"
    "openEuler-20.03-LTS|openEuler-20.03/openEuler-20.03LTS"
)

# RPM basename patterns to back-fill (gmp-c++ must come before gmp-devel,
# since gmp-devel Requires gmp-c++; order only matters for dnf, which sorts
# anyway, but we fetch in this order for log readability).
PATTERNS=( "gmp-c%2B%2B" "gmp-devel" )

fetch_one() {
    local repover="$1" subdir="$2" name="$3"
    if ls "$subdir"/${name}-*.rpm >/dev/null 2>&1; then
        return 0  # already present
    fi
    local pkgurl="$REPO/$repover/OS/aarch64/Packages/"
    local rpm
    # Escape regex metacharacters in the package name (e.g. the ++ in gmp-c++)
    # so grep -E matches it literally, then anchor to "-<version>.rpm".
    local escname
    escname=$(printf '%s' "$name" | sed 's/[+.]/\\&/g')
    rpm=$(curl -sSL "$pkgurl" 2>/dev/null | grep -oE "${escname}-[^\"]+\.rpm" | head -1)
    if [ -z "$rpm" ]; then
        echo "    $repover: $name NOT FOUND in OS repo" >&2
        return 1
    fi
    echo "    $repover: fetching $rpm"
    curl -fsSL -o "$subdir/$rpm" "$pkgurl$rpm"
}

for entry in "${ENTRIES[@]}"; do
    repover="${entry%%|*}"
    subdir="third-party/rpms/${entry##*|}"
    [ -d "$subdir" ] || { echo "  skip $repover (no tree dir)"; continue; }
    echo "== $repover =="
    # gmp-c++ must be present (gmp-devel Requires it); fetch in this order.
    fetch_one "$repover" "$subdir" "gmp-c++" || true
    fetch_one "$repover" "$subdir" "gmp-devel" || true
done

echo ""
echo "======== gmp-devel / gmp-c++ now present in: ========"
for d in third-party/rpms/openEuler-*/openEuler-*LTS*/; do
    gd=$(ls "$d"gmp-devel-*.rpm 2>/dev/null | wc -l)
    gc=$(ls "$d"gmp-c++-*.rpm 2>/dev/null | wc -l)
    echo "  $(basename "$d"): gmp-devel=$gd gmp-c++=$gc"
done
