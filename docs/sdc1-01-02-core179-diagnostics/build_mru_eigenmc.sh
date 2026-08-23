#!/bin/bash
# build_mru_eigenmc.sh — build the libc-only core 179 SDC minimal reproducer.
#
# Produces `mrueig`: a binary that links ONLY libc/libm (no libstdc++, no
# libgcc_s, no external libraries) yet runs the EXACT Eigen machine code
# (factorize_preordered rank-1 update: fmsub + indirect ldr/str + long-lived
# d4 accumulator + per-call malloc/free workspace) via a pure-C driver.
#
# The Eigen machine code is compiled from the Eigen 5.0 header-only templates
# with -fno-exceptions -fno-rtti; operator new/delete and __throw_length_error
# are provided locally (-> malloc/free/abort), so NO libstdc++ symbols remain.
# The final `ldd mrueig` shows only libc + libm.
#
# Reproduces core 179 SDC under same-socket 47-core load:
#   - 179 under load: 6-18 / 2500-3000 fails (multi-bit, elem[0], data-aliasing)
#   - 176 under load: 0 fails   (healthy core)
#   - 179 single-core: 0 fails  (load-triggered)
#   - 179 offline: 0 fails on other cores (isolation effective)
#
# Usage:
#   ./build_mru_eigenmc.sh           # builds ./mrueig in CWD
#   taskset -c 179 ./mrueig 3000 12345
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
EIG="${EIG:-$HOME/arm64-sdc-fuzzing/opendcdiag/third-party}"

# 1. compile Eigen machine-code object (C-ABI wrapper, -fno-exceptions -fno-rtti)
g++ -O2 -std=gnu++17 -march=armv8.1-a+crc+crypto -I"$EIG" \
    -fno-exceptions -fno-rtti -fPIC -c \
    "$HERE/eigen_cabidrv.cpp" -o "$HERE/eigen_cabidrv.o"

# 2. link pure-C driver + Eigen object -> libc-only binary
gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17 -ffp-contract=fast \
    "$HERE/mru_eigenmc.c" "$HERE/eigen_cabidrv.o" -o "$HERE/mrueig" -lm

echo "built: $HERE/mrueig"
echo "ldd:"; ldd "$HERE/mrueig"
