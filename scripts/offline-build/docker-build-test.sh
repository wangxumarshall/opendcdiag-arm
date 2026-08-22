#!/bin/bash
# docker-build-test.sh — in an openEuler container: offline-install the RPM
# deps for that OS version, build OpenDCDiag-arm, and run the test suite.
#
# Images are pre-loaded by pull-images.sh as openeuler-offline:<tag>, where
# <tag> matches the .os-version tag in third-party/rpms/<series>/<ver>/.
# (e.g. openeuler-offline:24.03-LTS-SP3 <-> openEuler-24.03LTS_SP3).
#
# Usage: docker-build-test.sh <image-tag> <rpm-series-dir>
#   <image-tag>     e.g. 24.03-LTS-SP3   (the openeuler-offline: tag)
#   <rpm-series-dir> e.g. openEuler-24.03 (under third-party/rpms/)
#
# The container is rootless podman; the openEuler image runs as root inside,
# so dnf works without sudo. Source is mounted read-only; builddir uses a
# per-version named volume so builds don't interfere.
set -euo pipefail

TAG="$1"
SERIES="$2"
REPO="$(git rev-parse --show-toplevel 2>/dev/null || echo "$PWD")"
IMG="openeuler-offline:${TAG}"
RPMDIR="$REPO/third-party/rpms/${SERIES}"

if ! podman image inspect "$IMG" >/dev/null 2>&1; then
    echo "错误: 镜像 $IMG 未加载。先运行 ./scripts/offline-build/pull-images.sh" >&2
    exit 1
fi
if [ ! -d "$RPMDIR" ]; then
    echo "错误: RPM 系列目录不存在: $RPMDIR" >&2
    exit 1
fi

# Map image tag (24.03-LTS-SP3) -> .os-version string (openEuler-24.03LTS_SP3).
# Format: <major>.03-LTS or <major>.03-LTS-SP<N> -> openEuler-<major>.03LTS[_SPN]
case "$TAG" in
    *-LTS-SP[0-9])
        sp="${TAG##*-SP}"
        major="${TAG%%.*}"
        OE_VER="openEuler-${major}.03LTS_SP${sp}"
        ;;
    *-LTS)
        major="${TAG%%.*}"
        OE_VER="openEuler-${major}.03LTS"
        ;;
    *) echo "无法识别的 tag 格式: $TAG" >&2; exit 1 ;;
esac

echo "============================================"
echo "  镜像: $IMG   期望版本: $OE_VER"
echo "  RPM 系列: $SERIES"
echo "============================================"

# Container script: detect OS, find matching RPM dir, install deps, build, test.
podman run --rm \
    -v "$REPO:/src:ro" \
    -v "$RPMDIR:/rpms:ro" \
    -v "buildcache-${TAG}:/src/builddir" \
    --privileged \
    "$IMG" /bin/bash -c '
set -euo pipefail
echo "--- 容器内 /etc/os-release ---"
head -3 /etc/os-release

# Copy offline-build scripts to a writable tmp dir (the repo is mounted ro)
mkdir -p /tmp/ob
cp /src/scripts/offline-build/_common.sh /src/scripts/offline-build/install-deps.sh \
   /src/scripts/offline-build/build.sh /tmp/ob/ 2>/dev/null || true
cp /src/scripts/offline-build/*.sh /tmp/ob/ 2>/dev/null || true
chmod +x /tmp/ob/*.sh

# Self-detect the OS version string, then find the matching RPM subdir.
source /tmp/ob/_common.sh
MY_VER=$(detect_os_version_full)
echo "容器自检版本串: $MY_VER"

MATCH_DIR=""
for d in /rpms/openEuler-*/; do
    [ -f "$d/.os-version" ] || continue
    tag=$(cat "$d/.os-version" | tr -d "[:space:]")
    if [ "$tag" = "$MY_VER" ]; then MATCH_DIR="$d"; break; fi
done
if [ -z "$MATCH_DIR" ]; then
    echo "错误: 在 /rpms 下未找到与容器 $MY_VER 匹配的 .os-version 目录" >&2
    echo "可用版本:" >&2
    for d in /rpms/openEuler-*/; do [ -f "$d/.os-version" ] && echo "  $(cat "$d/.os-version")" >&2; done
    exit 1
fi
echo "匹配 RPM 目录: $MATCH_DIR ($(ls "$MATCH_DIR"/*.rpm 2>/dev/null | wc -l) RPM)"

echo "--- 离线装依赖 (install-deps.sh) ---"
/tmp/ob/install-deps.sh "$MATCH_DIR" 2>&1 | tail -25

echo "--- 构建 (build.sh) ---"
cd /src
/tmp/ob/build.sh /src /src/builddir 2>&1 | tail -30

echo "--- 运行验证 ---"
echo -n "list-tests 行数: "; /src/builddir/opendcdiag --list-tests 2>/dev/null | wc -l
/src/builddir/opendcdiag -e zstd19 -t 2000 -n 1 2>&1 | grep -iE "exit|result" | head -3
echo "--- $MY_VER 完成 ---"
' 2>&1
