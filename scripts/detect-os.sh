#!/bin/sh
# detect-os.sh — print a concise description of the host OS for meson configure
# time decisions. Output is a single line: "openeuler 24.03 24 sp3" (or just the
# fields that are detectable). Intended to be called via meson run_command and
# parsed by splitting on space.
#
# Fields printed (space-separated), in order:
#   1. id            — lower-case OS id, e.g. "openeuler" / "ubuntu" / ""
#   2. version_id    — e.g. "24.03" / "20.03" / ""
#   3. major_int     — numeric major version (24 / 22 / 20) / "0" if unknown
#   4. sp            — short sp name e.g. "sp3" / "" for base release
#
# Used by meson.build to set OPENDCDIAG_OPENEULER_MAJOR and pick cpp_compat.
# SPDX-License-Identifier: Apache-2.0

set -eu

id=""
version_id=""

if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release 2>/dev/null || true
fi

id=${ID:-}
id=$(echo "$id" | tr '[:upper:]' '[:lower:]')
version_id=${VERSION_ID:-}

# numeric major (24.03 -> 24, 20.03 -> 20, 22.03 -> 22)
major_int=0
case "$version_id" in
    24.*) major_int=24 ;;
    22.*) major_int=22 ;;
    20.*) major_int=20 ;;
esac

# SP from VERSION string: "24.03 (LTS-SP3)" -> sp3 ; base LTS -> ""
sp=""
if [ -n "${VERSION:-}" ]; then
    sp=$(echo "$VERSION" | sed -nE 's/.*LTS[-_]?(SP[0-9]+).*/\1/p' | tr '[:upper:]' '[:lower:]')
fi

echo "$id $version_id $major_int $sp"
